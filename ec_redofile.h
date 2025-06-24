/*!
\file ec_redofile.h
\author jiangyong
\email  kipway@outlook.com
\update 
  2024.11.12 support no ec_alloctor


redofile
	按照块存储的redo日志文件，写入采用append模式。

eclib 3.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include <functional>
#include "ec_string.h"
#include "ec_stream.h"
#include "ec_log.h"
#include "ec_crc.h"
#ifndef _REDO_USE_FOPEN  // 使用C标准文件fopen函数， 否则使用ec::File(低级的open函数)
#include "ec_file.h"
#endif
#define SIZE_REDO_BLKSRC (1024 * 1024 * 4 - 256) //redo块原始大小最大数 4M
#define MAGIC_REDO_BLKFLAG 0xD387

#define REDOLOG_FILE_READONLY  0 //只读打开
#define REDOLOG_FILE_APPEND  1 //追加打开

#define REDOLOG_BLKCOMP_NONE  0 //不压缩
#define REDOLOG_BLKCOMP_LZ4   1 //LZ4压缩,先不支持
#define REDOLOG_BLKCOMP_ZLIB  2 //ZLIB压缩

#ifndef SECONDS_REDOFILE
#define SECONDS_REDOFILE 600 //每个redo文件存储的时间(秒),按照这个数对齐
#endif
#define SIZE_REDOLOG_BLKHEAD 16 //redo日志块头部大小

#define REDO_BLKORD_SNAP  0x01 //快照
#define REDO_BLKORD_HISI  0x02 //Lishi补录

#ifndef REDO_ZLIB_BLKSIZE
#define REDO_ZLIB_BLKSIZE 1024  //只用zilb压缩是最小块的大小
#endif

#ifndef REDO_DEFAULT_PRE //默认redo文件前缀
#define REDO_DEFAULT_PRE "dbredo_"
#endif
namespace ec {

	/**
	 * @brief 块头部,按照小头存储，16字节固定大小
	*/
	class redoblk_head
	{
	public:
		uint16_t _magic;//魔数, MAGIC_REDO_BLKFLAG
		uint8_t  _compress;//压缩方式; 0: None; 1:LZ4
		uint8_t  _blktype;//块类型,应用层定义
		uint32_t _sizesrc;//块未原始长度(未压缩时)
		uint32_t _sizebody;//块长度，不含头部
		uint32_t _ucrc32; //
	public:
		redoblk_head() : _magic(MAGIC_REDO_BLKFLAG)
			, _compress(REDOLOG_BLKCOMP_NONE), _blktype(0), _sizesrc(0), _sizebody(0), _ucrc32(0) {
		}
		void encode(ec::stream& ss)
		{
			ss.setpos(0);
			ss << _magic << _compress << _blktype << _sizesrc << _sizebody;
			_ucrc32 = ec::crc32(ss.getp(), (uint32_t)ss.getpos());
			ss << _ucrc32;
		}

		bool decode(ec::stream& ss)
		{
			ss >> _magic >> _compress >> _blktype >> _sizesrc >> _sizebody >> _ucrc32;
			uint32_t uc32 = ec::crc32(ss.getp(), 12);
			return (uc32 == _ucrc32) && (MAGIC_REDO_BLKFLAG == _magic);
		}
	};

	class redofile
	{
	protected:
		int _redomode = REDOLOG_FILE_APPEND;
		int _usezlib = 1; //默认开启zlib压缩
		int64_t _timet;//时标自1970-1-1开始的秒数
		std::mutex* _pmutex = nullptr;
		ec::string _path; //已规格化,后带'/', utf8编码
		ec::string _filenamepre; //文件名前缀,默认
#ifdef _REDO_USE_FOPEN
		FILE* _pfile = nullptr;
#else
		ec::File _file;
#endif
	private:
		ec::string _sfile;
	public:
		redofile(int redomode, std::mutex* pmutex, const char* spath, int usezlib = 1, const char* snamepre = nullptr) 
			: _redomode(redomode), _timet(0), _pmutex(pmutex) {
			if (snamepre && *snamepre)
				_filenamepre = snamepre;
			init(spath, usezlib);
		}

		~redofile() {
			Close();
		}

		void Close() {
#ifdef _REDO_USE_FOPEN
			if (_pfile) {
				fclose(_pfile);
				_pfile = nullptr;
			}
#else
			_file.Close();
#endif
		}

		/**
		 * @brief 初始化目录
		 * @param spath 目录，utf8编码，已经规格化
		*/
		void init(const char* spath, int usezlib)
		{
			_usezlib = usezlib;
			if (spath && *spath)
				_path.assign(spath);
		}

	public:
		/**
		 * @brief 追加数据块，数据块为空或者0长度时，调用该函数切换输出文件。
		 * @param pblksrc 原始数据块,nullptr用于切换文件
		 * @param srclen pblksrc的字节数，0用于切换文件
		 * @return 0:success; -1:failed.
		*/
		int appendblk(const void* pblksrc, size_t srclen, uint8_t blktype) 
		{			
			ec::unique_lock lck(_pmutex);
			if (srclen > SIZE_REDO_BLKSRC
				|| REDOLOG_FILE_APPEND != _redomode
				|| _path.empty()
				)
				return -1;
			int64_t ltime = ::time(nullptr);
			if (ltime % SECONDS_REDOFILE)
				ltime -= ltime % SECONDS_REDOFILE; //前对齐
			if (_timet != ltime) { //换文件
#ifdef _REDO_USE_FOPEN
				if (_pfile) {
					fclose(_pfile);
					_pfile = nullptr;
				}
#else
				if (_file.IsOpen())
					_file.Close();
#endif
				_timet = ltime;
			}

			if (!pblksrc || !srclen)
				return 0; //用于切换文件。
#ifdef _REDO_USE_FOPEN
			if (nullptr == _pfile) { //打开文件
#else
			if (!_file.IsOpen()) { //打开文件
#endif	
				char sfile[64];
				const char* spre = _filenamepre.empty() ? REDO_DEFAULT_PRE : _filenamepre.c_str();
				size_t zlen = snprintf(sfile, sizeof(sfile), "%s%jd", spre, _timet);
				if (zlen >= sizeof(sfile))
					return -1;
				_sfile = _path;
				_sfile.append(sfile, zlen);
#ifdef _REDO_USE_FOPEN
#ifdef _WIN32
				UINT codepage = ec::strisutf8(_sfile.c_str()) ? CP_UTF8 : CP_ACP;
				wchar_t wsfile[_MAX_PATH] = { 0 };
				if (!MultiByteToWideChar(codepage, 0, _sfile.c_str(), -1, wsfile, sizeof(wsfile) / sizeof(wchar_t)))
					return -1;
				_pfile = _wfopen(wsfile, L"ab");
#else
				_pfile = fopen(_sfile.c_str(), "ab");
#endif
				if (!_pfile)
					return -1;

#else
				if (!_file.Open(_sfile.c_str(), ec::File::OF_CREAT | ec::File::OF_WRONLY | ec::File::OF_APPEND_DATA)) {
					if (!_file.Open(_sfile.c_str(), ec::File::OF_WRONLY | ec::File::OF_APPEND_DATA))
						return -1;
					_file.Seek(0, ec::File::seek_end);
				}
#endif
			}

			redoblk_head h;
			char shead[SIZE_REDOLOG_BLKHEAD] = { 0 };
			ec::stream ss(shead, SIZE_REDOLOG_BLKHEAD);
			h._blktype = blktype;
			h._sizesrc = (uint32_t)srclen;
			if (h._sizesrc < REDO_ZLIB_BLKSIZE || !_usezlib) {//不压缩
				h._sizebody = (uint32_t)srclen;
				h._compress = REDOLOG_BLKCOMP_NONE;
				h.encode(ss);
#ifdef _REDO_USE_FOPEN
				if (!fwrite(shead, SIZE_REDOLOG_BLKHEAD, 1, _pfile))
					return -1;
				if (!fwrite(pblksrc, srclen, 1, _pfile))
					return -1;
				fflush(_pfile);
#else
				if (_file.Write(shead, SIZE_REDOLOG_BLKHEAD) < 0)
					return -1;
				if (_file.Write(pblksrc, (uint32_t)srclen) < 0)
					return -1;
#endif				
			}
			else { // zlib
				ec::bytes zout;
				zout.reserve(h._sizesrc);
				if (0 != ec::ws_encode_zlib(pblksrc, srclen, &zout))
					return -1;
				h._sizebody = (uint32_t)zout.size();
				h._compress = REDOLOG_BLKCOMP_ZLIB;
				h.encode(ss);
#ifdef _REDO_USE_FOPEN
				if (!fwrite(shead, SIZE_REDOLOG_BLKHEAD, 1, _pfile))
					return -1;
				if (!fwrite(zout.data(), zout.size(), 1, _pfile))
					return -1;
				fflush(_pfile);
#else
				if (_file.Write(shead, SIZE_REDOLOG_BLKHEAD) < 0)
					return -1;
				if (_file.Write(zout.data(), (uint32_t)zout.size()) < 0)
					return -1;
#endif				
			}
			return 0;
		}

		/**
		 * @brief 扫描并处理块
		 * @param ltime
		 * @param plog
		 * @param fun 返回false将会终止扫描块
		 * @return 记录数，-1表示有错误
		*/
		int redo_onefile(int64_t ltime, ec::ilog * plog, std::function <bool(char* ps, size_t zlen, uint8_t blktype)> fun)
		{
#ifdef _REDO_USE_FOPEN
			if (_pfile) {
				fclose(_pfile);
				_pfile = nullptr;
			}
#else
			if (_file.IsOpen())
				_file.Close();
#endif

			char sfile[64];
			const char* spre = _filenamepre.empty() ? REDO_DEFAULT_PRE : _filenamepre.c_str();
			size_t zlen = snprintf(sfile, sizeof(sfile), "%s%jd", spre, ltime);
			if (zlen >= sizeof(sfile))
				return -1;
			_sfile = _path;
			_sfile.append(sfile, zlen);
#ifdef _REDO_USE_FOPEN
#ifdef _WIN32
			UINT codepage = ec::strisutf8(_sfile.c_str()) ? CP_UTF8 : CP_ACP;
			wchar_t wsfile[_MAX_PATH] = { 0 };
			if (!MultiByteToWideChar(codepage, 0, _sfile.c_str(), -1, wsfile, sizeof(wsfile) / sizeof(wchar_t)))
				return -1;
			_pfile = _wfopen(wsfile, L"rb");
#else
			_pfile = fopen(_sfile.c_str(), "rb");
#endif
			if (!_pfile) {
				if (plog)
					plog->add(CLOG_DEFAULT_ERR, "open redo file %s failed.", _sfile.c_str());
				return -1;
			}
#else
			if (!_file.Open(_sfile.c_str(), ec::File::OF_RDONLY))
				return -1;
#endif

			char shead[SIZE_REDOLOG_BLKHEAD] = { 0 };
			ec::stream ss(shead, SIZE_REDOLOG_BLKHEAD);
			redoblk_head h;

			int numblks = 0;
			char* psrc = nullptr;
			for (;;) {
#ifdef _REDO_USE_FOPEN
				if (16u != fread(shead, 1, SIZE_REDOLOG_BLKHEAD, _pfile)) {
					break;
				}
#else
				if (16 != _file.Read(shead, SIZE_REDOLOG_BLKHEAD))
					break;
#endif
				ss.setpos(0);
				if (!h.decode(ss)) {
					if (plog)
						plog->add(CLOG_DEFAULT_ERR, "redo file %s blk error.", _sfile.c_str());
					numblks = -1;
					break;
				}
				if (!h._sizebody)
					continue;
				ec::autobuf<char> blk(h._sizebody);
				ec::bytes src;
#ifdef _REDO_USE_FOPEN
				if (h._sizebody != fread(blk.data(), 1, h._sizebody, _pfile)) {
					if (plog)
						plog->add(CLOG_DEFAULT_ERR, "redo file %s read block body error.", _sfile.c_str());
					numblks = -1;
					break;
				}
#else
				if (h._sizebody != (uint32_t)_file.Read(blk.data(), h._sizebody)) {
					if (plog)
						plog->add(CLOG_DEFAULT_ERR, "redo file %s read block body error.", _sfile.c_str());
					numblks = -1;
					break;
				}
#endif				
				if (REDOLOG_BLKCOMP_NONE == h._compress) {
					psrc = blk.data();
				}
				else if (REDOLOG_BLKCOMP_ZLIB == h._compress) {
					src.reserve(h._sizesrc + 8);
					if (0 != ec::ws_decode_zlib(blk.data(), h._sizebody, &src)) {
						numblks = -1;
						break;
					}
					psrc = (char*)src.data();
					if (h._sizesrc != src.size()) {
						if (plog)
							plog->add(CLOG_DEFAULT_WRN, "redo file %s block sizesrc error compress %u. decode zlib size %zu, head sizesrc=%u",
								_sfile.c_str(), h._compress, src.size(), h._sizesrc);
						h._sizesrc = (uint32_t)src.size();
					}
				}
				else {
					if (plog)
						plog->add(CLOG_DEFAULT_ERR, "redo file %s unkown block body compress %u.", _sfile.c_str(), h._compress);
					numblks = -1;
					break;
				}
				if (!fun(psrc, h._sizesrc, h._blktype)) {
					numblks = -1;
					break;
				}
				++numblks;
			}
			if (plog)
				plog->add(CLOG_DEFAULT_MSG, "redo file %s success, number objects %d.", _sfile.c_str(), numblks);
#ifdef _REDO_USE_FOPEN
			fclose(_pfile);
			_pfile = nullptr;
#else
			_file.Close();
#endif
			return numblks;
		}
	};
}//namespace ec