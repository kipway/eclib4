/*!
\brief ec_objfile.h
object file

\author	jiangyong
\email  kipway@outlook.com
\update 
  2023-12-14 增加表空间大小获取

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once

#include <string>
#include <assert.h>
#include <stdint.h>
#include <functional>
#include <algorithm>
#include "ec_diskio.h"
#include "ec_file.h"
#include "ec_log.h"
#include "ec_stream.h"
#include "ec_crc.h"
#include "ec_queue.h"

namespace ec {
	constexpr uint32_t OBF_MAGIC = 0xa733b1a8;//rdf magic number
	constexpr uint32_t OBF_VERSION = 0x10000; //rdf version

	constexpr uint32_t OBF_MIN_PGSIZE = 128; //min page size
	constexpr uint32_t OBF_MAX_PGSIZE = (1024 * 16); //max page size
	constexpr uint16_t OBF_PAGE_MAGIC = 0xC1C2; // empty page head magic
	constexpr uint16_t OBF_PAGE_FREE = 0xf5A0; // free page flag
	constexpr uint16_t OBF_PAGE_FIRST = 0xf5A1; // first data page flag
	constexpr uint16_t OBF_PAGE_NEXT = 0xf5A2; // next data page flag
	constexpr uint32_t OBF_PAGE_HEADSIZE = 16;  // page head info size

	constexpr int32_t OBF_HEADPAGE_SIZE = 8192; // head page size
	constexpr int32_t OBF_PARAM_SIZE = 128;
	constexpr int32_t OBF_DYNA_POS = 4096; //dynamic info start position
	constexpr int32_t OBF_INFO_SIZE = 32;


	constexpr uint32_t OBF_PAGE_END = 0xFFFFFFFF;
	constexpr int OBF_GROW_SIZE = 256;
	enum obf_error {
		obf_ok = 0,
		obf_err_failed = 1,
		obf_err_exist = 2,
		obf_err_isopen = 3,
		obf_err_param = 4,
		obf_err_createdir = 5,
		obf_err_createfile = 6,
		obf_err_openfile = 7,
		obf_err_read = 8,
		obf_err_write = 9,
		obf_err_seek = 10,
		obf_err_headcheck = 11,
		obf_err_name = 13,
		obf_err_pghead = 15,
		obf_err_notfirst = 16
	};

	// table space static infomation, 128 bytes, save in 0 offset of each file
	class obf_param //
	{
	public:
		//static info
		uint32_t _magic;
		uint32_t _verson;
		char     _name[96]; //application magic
		uint8_t  _res[16];
		uint32_t _pagesize;
		uint32_t _crc32;
		//sizeof = 128

		obf_param()
			: _magic(OBF_MAGIC)
			, _verson(OBF_VERSION)
			, _pagesize(512)
			, _crc32(0)
		{
			memset(_name, 0, sizeof(_name));
			memset(_res, 0, sizeof(_res));
		}

		// serialize to buffer, return size , 0: error
		size_t serialize(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			try {
				ss << _magic << _verson;
				ss.write(_name, sizeof(_name));
				ss.write(_res, sizeof(_res));
				ss << _pagesize;

				_crc32 = ec::crc32(pout, (uint32_t)ss.getpos());
				ss << _crc32;
			}
			catch (...) {
				return 0;
			}
			return ss.getpos();
		}

		// return 0:ok; -1:error
		int parse(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			uint32_t uc;
			try {
				ss >> _magic >> _verson;
				ss.read(_name, sizeof(_name));
				ss.read(_res, sizeof(_res));
				ss >> _pagesize;

				uc = ec::crc32(pout, (uint32_t)ss.getpos());
				ss >> _crc32;
				if (uc != _crc32 || _magic != OBF_MAGIC)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	// free page head info 16bytes, link table, save in 0 offset each free page
	class obf_pagehead
	{
	public:
		//link info
		uint16_t _magic; // OBF_PAGE_MAGIC = 0xc1c2;
		uint16_t _pgflag; // OBF_PAGE_FREE,OBF_PAGE_FIRST,OBF_PAGE_NEXT
		uint32_t _pgnonext; // next page no; OBF_PAGE_END: no next page
		uint16_t _pgdatasize; // data size in this page;
		uint16_t _usres; // 0 reserved
		uint32_t _crc32;
		//sizeof = 16

		obf_pagehead(uint16_t upgflag = OBF_PAGE_FREE)
			: _magic(OBF_PAGE_MAGIC)
			, _pgflag(upgflag)
			, _pgnonext(OBF_PAGE_END)
			, _pgdatasize(0)
			, _usres(0)
			, _crc32(0)
		{
		}

		// serialize to buffer, return size , 0: error
		size_t serialize(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			try {
				ss << _magic << _pgflag << _pgnonext << _pgdatasize << _usres;
				_crc32 = ec::crc32(pout, (uint32_t)ss.getpos());
				ss << _crc32;
			}
			catch (...) {
				return 0;
			}
			return ss.getpos();
		}

		// return 0:ok; -1:error
		int parse(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			uint32_t uc;
			try {
				ss >> _magic >> _pgflag >> _pgnonext >> _pgdatasize >> _usres;
				uc = ec::crc32(pout, (uint32_t)ss.getpos());
				ss >> _crc32;
				if (uc != _crc32 || _magic != OBF_PAGE_MAGIC)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	//table space dynamic information, save in TBS_DYNA_POS offset of tbs first file
	class obf_info
	{
	public:
		//dynamic info
		uint32_t _magic;
		uint32_t _verson;
		uint32_t  _numallpages;// number of all pages, grown 1024 pages once
		uint32_t  _nextpageno; // next free page No, -1: no free page
		uint32_t  _numfreepages;// number of free pages
		uint8_t  _res[8];
		uint32_t _crc32;
		//sizeof = 32

		obf_info()
			: _magic(OBF_MAGIC)
			, _verson(OBF_VERSION)
			, _numallpages(0)
			, _nextpageno(OBF_PAGE_END)
			, _numfreepages(0)
			, _crc32(0)
		{
			memset(_res, 0, sizeof(_res));
		}

		// serialize to buffer, return size , 0: error
		size_t serialize(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			try {
				ss << _magic << _verson << _numallpages << _nextpageno << _numfreepages;
				ss.write(_res, sizeof(_res));
				_crc32 = ec::crc32(pout, (uint32_t)ss.getpos());
				ss << _crc32;
			}
			catch (...) {
				return 0;
			}
			return ss.getpos();
		}

		// return 0:ok; -1:error
		int parse(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			uint32_t uc;
			try {
				ss >> _magic >> _verson >> _numallpages >> _nextpageno >> _numfreepages;
				ss.read(_res, sizeof(_res));
				uc = ec::crc32(pout, (uint32_t)ss.getpos());
				ss >> _crc32;
				if (uc != _crc32 || _magic != OBF_MAGIC)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	class objfile : public File
	{
	protected:
		int _lasterr;
		ec::ilog* _plog;
		std::string _sfile;
		obf_param _args;
		obf_info  _info;
	public:
		objfile(ec::ilog* plog = nullptr)
			: _lasterr(0)
			, _plog(plog)
		{
		}

		inline void setLog(ec::ilog* plog) {
			_plog = plog;
		}

		inline ec::ilog* getLog()
		{
			return _plog;
		}

		inline int getLastErr()
		{
			return _lasterr;
		}

		int64_t sizeTabspace()
		{
			return (int64_t)_args._pagesize * (int64_t)_info._numallpages;
		}
		/*!
		 \brief	Create record file

		 \param 	sdbfile 	The DB file name, utf8.
		 \param 	sappflag	The applaction flag. < 32 chars
		 \param 	pagesize 	The page size. [128, 1024 * 16],default 512
		 \param		bsync		true:wirte througt; false wirte back; default is false
		 \return	0:success. -1:error.
		 \remark    if sdbfile is exist return -1;
		 */
		int createFile(const char* sdbfile, const char* sappflag, uint32_t pagesize = 512, bool bsync = false)
		{
			if (pagesize < OBF_MIN_PGSIZE || pagesize > OBF_MAX_PGSIZE || (sappflag && strlen(sappflag) >= sizeof(_args._name))) {
				_lasterr = obf_err_param;
				return -1;
			}
			File::Close();
			_args._pagesize = pagesize;
			ec::strlcpy(_args._name, sappflag, sizeof(_args._name));

			uint32_t uflag = OF_RDWR | OF_CREAT;
			if (bsync)
				uflag |= OF_SYNC;
			if (!File::Open(sdbfile, uflag, OF_SHARE_READ)) {
				_lasterr = obf_err_createfile;
				return -1;
			}
			do {
				unique_filelock flck(this, 0, 0, true);
				char pg[OBF_HEADPAGE_SIZE] = { 0 };

				_args.serialize(pg, OBF_PARAM_SIZE);
				_info.serialize(&pg[OBF_DYNA_POS], OBF_INFO_SIZE);

				if (Write(pg, OBF_HEADPAGE_SIZE) < 0) {
					_lasterr = obf_err_write;
					File::Close();
					return -1;
				}
				flush();
			} while (0);
			_sfile = sdbfile;
			_lasterr = obf_ok;
			return 0;
		}

		/*!
		\brief open an exist file
		\return 0:ok; -1:error
		*/
		int openFile(const char* sdbfile, const char* sappflag = nullptr, bool bsync = false)
		{
			uint32_t uflag = OF_RDWR;
			if (bsync)
				uflag |= OF_SYNC;
			if (!File::Open(sdbfile, uflag, OF_SHARE_READ)) {
				_lasterr = obf_err_openfile;
				return -1;
			}
			do {
				unique_filelock flck(this, 0, 0, false);
				char pg[OBF_HEADPAGE_SIZE] = { 0 };
				if (OBF_HEADPAGE_SIZE != Read(pg, OBF_HEADPAGE_SIZE)) {
					_lasterr = obf_err_read;
					File::Close();
					return -1;
				}
				if (_args.parse(pg, OBF_PARAM_SIZE) < 0 || _info.parse(&pg[OBF_DYNA_POS], OBF_INFO_SIZE) < 0) {
					_lasterr = obf_err_headcheck;
					File::Close();
					return -1;
				}
				if (sappflag && !ec::streq(sappflag, _args._name)) {
					_lasterr = obf_err_name;
					File::Close();
					return -1;
				}
			} while (0);
			_sfile = sdbfile;
			_lasterr = obf_ok;
			return 0;
		}

		/**
		 * @brief insert or update object
		 * @param pdata
		 * @param datasize
		 * @param pgnoFirst OBF_PAGE_END: need alloc first page, else is the object first pageno, update it
		 * @return 0:success; -1:error;
		*/
		int writeObject(const void* pdata, int datasize, uint32_t& pgnoFirst)
		{
			const char* pd = (const char*)pdata;
			int nleft = datasize, nw = (int)(_args._pagesize - OBF_PAGE_HEADSIZE), nret = 0;
			uint32_t pgcur = pgnoFirst, pgnxt;
			ec::queue<uint32_t> pages;
			if (getPages(pgnoFirst, pages) < 0)
				return -1;
			pgcur = pgalloc(pages);
			if (OBF_PAGE_END == pgnoFirst) {
				pgnoFirst = pgcur;
			}
			do {
				if (nleft <= nw) { // end
					nret = writepage(pgcur, (pgcur == pgnoFirst) ? OBF_PAGE_FIRST : OBF_PAGE_NEXT, pd, nleft, OBF_PAGE_END);
					break;
				}
				pgnxt = pgalloc(pages);
				if (OBF_PAGE_END == pgnxt) {
					nret = -1;
					break;
				}
				if (writepage(pgcur, (pgcur == pgnoFirst) ? OBF_PAGE_FIRST : OBF_PAGE_NEXT, pd, nw, pgnxt) < 0) {
					nret = -1;
					break;
				}
				pd += nw;
				nleft -= nw;
				pgcur = pgnxt;
			} while (nleft > 0);
			freepages(pages);
			if (0 == nret)
				_lasterr = 0;
			return nret;
		}

		/**
		 * @brief get object to vout
		 * @param pgnoStart
		 * @param vout
		 * @return
		*/
		template<class _CLS = ec::bytes>
		int getObject(uint32_t pgnoStart, _CLS& vout)
		{
			int nr = 0;
			uint32_t pgno = pgnoStart;
			obf_pagehead hd;
			uint8_t pg[OBF_MAX_PGSIZE];
			while (OBF_PAGE_END != pgno) {
				nr = ReadFrom(pagepos(pgno), pg, _args._pagesize);
				if (nr < 0) {
					_lasterr = obf_err_read;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%u,error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (hd.parse(pg, OBF_PAGE_HEADSIZE) < 0) {
					_lasterr = obf_err_headcheck;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%d, head parse error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (hd._pgflag != OBF_PAGE_FIRST && hd._pgflag != OBF_PAGE_NEXT) {
					_lasterr = obf_err_pghead;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%d, head flag error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (pgnoStart == pgno && hd._pgflag != OBF_PAGE_FIRST) {
					_lasterr = obf_err_notfirst;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%d, not first page flag error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (hd._pgdatasize + OBF_PAGE_HEADSIZE > _args._pagesize || hd._pgdatasize + OBF_PAGE_HEADSIZE > OBF_MAX_PGSIZE) {
					_lasterr = obf_err_pghead;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%d, head datasize error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				vout.append(&pg[OBF_PAGE_HEADSIZE], hd._pgdatasize);
				pgno = hd._pgnonext;
			}
			return 0;
		}

		/**
		 * @brief delete object form start pageno
		 * @param pgnoStart start page No.
		 * @return 0:success; -1:error;
		*/
		int freeObject(uint32_t pgnoStart)
		{
			ec::queue<uint32_t> pages;
			if (getPages(pgnoStart, pages) < 0)
				return -1;
			freepages(pages);
			return 0;
		}

		/**
		 * @brief load all objects
		 * @param fun
		 * @return number objects
		*/
		int loadAll(std::function<void(void* prec, size_t recsize, uint32_t positon)> fun)
		{
			uint32_t i, n = 0;
			obf_pagehead hd;
			int64_t lfpos = OBF_HEADPAGE_SIZE;
			ec::bytes vout;
			uint8_t pg[OBF_MAX_PGSIZE];
			vout.reserve(_args._pagesize);
			for (i = 0; i < _info._numallpages; i++) {
				if (ReadFrom(lfpos, pg, OBF_PAGE_HEADSIZE) != OBF_PAGE_HEADSIZE)
					break;
				lfpos += _args._pagesize;
				if (hd.parse(pg, OBF_PAGE_HEADSIZE) < 0) {
					_lasterr = obf_err_headcheck;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read pagehead pgno=%u, head parse error(%d).",
							_sfile.c_str(), i, _lasterr);
					continue;
				}
				if (hd._pgflag == OBF_PAGE_FIRST) {
					vout.clear();
					if (getObject(i, vout) < 0)
						continue;
					fun(vout.data(), vout.size(), i);
					n++;
				}
			}
			return n;
		}

		void logInfo()
		{
			if (!_plog)
				return;
			_plog->add(CLOG_DEFAULT_DBG, "object file %s info:\n  Version  : %u.%u.%u\n  PageSize : %u  \n  AllPages : %u\n  FreePages: %u",
				_sfile.c_str(), (_info._verson >> 16) & 0xFF, (_info._verson >> 8) & 0xFF, _info._verson & 0xFF,
				_args._pagesize, _info._numallpages, _info._numfreepages
			);
		}

		/**
		 * @brief get object pages
		 * @tparam _CLS  ec::queue
		 * @param pgnoStart object start page No.
		 * @param pages output pages
		 * @return 0:success; -1:error
		*/
		template<class _CLS = ec::queue<uint32_t>>
		int getPages(uint32_t pgnoStart, _CLS& pages)
		{
			int nr = 0;
			uint32_t pgno = pgnoStart;
			obf_pagehead hd;
			uint8_t pg[OBF_PAGE_HEADSIZE];
			while (OBF_PAGE_END != pgno) {
				nr = ReadFrom(pagepos(pgno), pg, OBF_PAGE_HEADSIZE);
				if (nr < 0) {
					_lasterr = obf_err_read;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read pagehead pgno=%u,error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (hd.parse(pg, OBF_PAGE_HEADSIZE) < 0) {
					_lasterr = obf_err_headcheck;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read pagehead pgno=%d, head parse error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (pgnoStart == pgno && hd._pgflag != OBF_PAGE_FIRST) {
					_lasterr = obf_err_notfirst;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%d, not first page flag error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				if (hd._pgflag != OBF_PAGE_FIRST && hd._pgflag != OBF_PAGE_NEXT) {
					_lasterr = obf_err_pghead;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s read page pgno=%d, page not first or next flag error(%d).",
							_sfile.c_str(), pgno, _lasterr);
					return -1;
				}
				pages.push(pgno);
				pgno = hd._pgnonext;
			}
			return 0;
		}
	protected:
		inline long long pagepos(int pgno) {
			return static_cast<long long>(_args._pagesize) * pgno + OBF_HEADPAGE_SIZE;
		}

		//update table space infomation to first file position RDF_DYNA_POS
		int updateinfo()
		{
			uint8_t ubuf[OBF_INFO_SIZE];
			memset(ubuf, 0, sizeof(ubuf));
			_info.serialize(ubuf, sizeof(ubuf));
			if (WriteTo(OBF_DYNA_POS, ubuf, OBF_INFO_SIZE) < 0) {
				_lasterr = obf_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "object file %s write page info error(%d).", _sfile.c_str(), _lasterr);
				return -1;
			}
			_lasterr = obf_ok;
			if (_plog) {
				_plog->add(CLOG_DEFAULT_ALL, "update object file %s success, numpages=%d, numfreepages=%d, nextpgno=%d",
					_sfile.c_str(), _info._numallpages, _info._numfreepages, _info._nextpageno);
			}
			return 0;
		}

		// grown OBF_GROW_SIZE pages
		int grownpages()
		{
			if (Seek(pagepos(_info._numallpages), File::seek_set) < 0) {
				_lasterr = obf_err_seek;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "object file %s grown pages failed.", _sfile.c_str());
				return -1;
			}
			char buf[OBF_MAX_PGSIZE];
			memset(buf, -1, sizeof(buf));

			obf_pagehead hd;
			for (auto i = 0; i < OBF_GROW_SIZE; i++) {
				if (i + 1 == OBF_GROW_SIZE)
					hd._pgnonext = _info._nextpageno;// the last page
				else
					hd._pgnonext = _info._numallpages + i + 1;
				hd.serialize(buf, sizeof(buf));
				if (Write(buf, static_cast<uint32_t>(_args._pagesize)) < 0) {
					_lasterr = obf_err_write;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "object file %s grown write page failed.", _sfile.c_str());
					return -1;
				}
			}
			_info._nextpageno = _info._numallpages;
			_info._numallpages += OBF_GROW_SIZE;
			_info._numfreepages += OBF_GROW_SIZE;

			if (updateinfo() < 0)
				return -1;
			flush();
			return 0;
		}
	private:
		//alloc one page; return pgno, OBF_PAGE_END:error
		uint32_t pagealloc()
		{
			if (_info._nextpageno == OBF_PAGE_END) {
				if (grownpages() < 0)
					return OBF_PAGE_END;
			}
			uint8_t buf[OBF_PAGE_HEADSIZE];
			if (OBF_PAGE_HEADSIZE != ReadFrom(pagepos(_info._nextpageno), buf, OBF_PAGE_HEADSIZE)) {
				_lasterr = obf_err_pghead;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "object file %s page %d read head error(%d).",
						_sfile.c_str(), _info._nextpageno, _lasterr);
				}
				return OBF_PAGE_END;
			}
			obf_pagehead pgh;
			if (pgh.parse(buf, OBF_PAGE_HEADSIZE) < 0 || OBF_PAGE_FREE != pgh._pgflag) {
				_lasterr = obf_err_pghead;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "object file %s alloc page %d head parse error(%d).",
						_sfile.c_str(), _info._nextpageno, _lasterr);
				}
				return OBF_PAGE_END;
			}
			uint32_t pgno = _info._nextpageno;
			_info._nextpageno = pgh._pgnonext;
			_info._numfreepages -= 1;
			if (updateinfo() < 0)
				return OBF_PAGE_END;

			pgh._pgflag = OBF_PAGE_FIRST;
			pgh._pgdatasize = 0;
			pgh._pgnonext = OBF_PAGE_END;
			pgh.serialize(buf, OBF_PAGE_HEADSIZE);
			WriteTo(pagepos(pgno), buf, OBF_PAGE_HEADSIZE); //写入页面头，0长度数据,下次释放判断为非闲空页面。
			return pgno;
		}

		//return true is free page
		bool isfreepage(int32_t pgno)
		{
			uint8_t pghead[OBF_PAGE_HEADSIZE];
			int nr = ReadFrom(pagepos(pgno), pghead, OBF_PAGE_HEADSIZE);
			if (nr < 0) {
				_lasterr = obf_err_read;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "object file %s read page head pgno=%d read error(%d).",
						_sfile.c_str(), pgno, _lasterr);
				return false;
			}
			obf_pagehead pgh;
			if (!pgh.parse(pghead, OBF_PAGE_HEADSIZE))
				return false;
			return OBF_PAGE_FREE == pgh._pgflag;
		}

		// free one page; return 0:ok; -1:error
		int pagefree(uint32_t pgno)
		{
			if (pgno + 1 >= _info._numallpages) {
				_lasterr = obf_err_failed;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "free object file %s pgno(%d), uplimit error(%d).",
						_sfile.c_str(), pgno, _lasterr);
				}
				return -1;
			}
			if (isfreepage(pgno)) {
				if (_plog) {
					_plog->add(CLOG_DEFAULT_WRN, "object file %s pgno(%d), refree.",
						_sfile.c_str(), pgno);
				}
				return 0;
			}
			uint8_t buf[OBF_PAGE_HEADSIZE];
			obf_pagehead pgh;
			pgh._pgflag = OBF_PAGE_FREE;
			pgh._pgnonext = _info._nextpageno;
			pgh.serialize(buf, sizeof(buf));
			if (OBF_PAGE_HEADSIZE != WriteTo(pagepos(pgno), buf, OBF_PAGE_HEADSIZE)) {
				_lasterr = obf_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "object file %s free page %d head write error(%d)",
						_sfile.c_str(), _info._nextpageno, _lasterr);
				return -1;
			}
			_info._nextpageno = pgno;
			_info._numfreepages += 1;
			return updateinfo();
		}

		// write one page, return 0 success; -1:error
		int writepage(uint32_t pgno, uint16_t pgflag, const char* pdata, int datasize, uint32_t pgnxt)
		{
			uint8_t pg[OBF_MAX_PGSIZE];
			obf_pagehead hd;
			hd._pgflag = pgflag;
			hd._pgdatasize = (uint16_t)datasize;
			hd._pgnonext = pgnxt;

			hd.serialize(pg, OBF_MAX_PGSIZE);
			memcpy(&pg[OBF_PAGE_HEADSIZE], pdata, datasize);
			if (WriteTo(pagepos(pgno), pg, (uint32_t)(datasize + OBF_PAGE_HEADSIZE)) < 0) {
				_lasterr = obf_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "object file %s write page pgno=%d,size=%d write error(%d).",
						_sfile.c_str(), pgno, datasize, _lasterr);
				return -1;
			}
			return 0;
		}

		uint32_t pgalloc(ec::queue<uint32_t>& pages)
		{
			if (pages.empty())
				return  pagealloc();
			uint32_t ur = pages.front();
			pages.pop();
			return ur;
		}

		void freepages(ec::queue<uint32_t>& pages)
		{
			if (pages.size() < 2) {
				while (!pages.empty()) {
					pagefree(pages.front());
					pages.pop();
				}
				return;
			}
			ec::vector<uint32_t> vpages;
			while (!pages.empty()) {
				vpages.push_back(pages.front());
				pages.pop();
			}
			std::sort(vpages.begin(), vpages.end(), [](uint32_t v1, uint32_t v2) {return v2 < v1; });
			for (auto i : vpages)
				pagefree(i);
		}
	}; // objfile
}// namespace ec