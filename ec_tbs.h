/*!
\file ec_tbs.h
table space

\author	jiangyong
\email  kipway@outlook.com
\update
  2023-12-14 增加数据表空间获取
  2023-3-9 修正file_文件句柄缓冲; 更新pagealloc()，增加一次错误重新分配, 每次增长页面数改为256; 整理加代码注释

eclib 3.0 Copyright (c) 2017-2023, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

表空间由连续编号的文件组成,每个文件保留前面8K作为信息存储，第一个文件的前8K还保存空闲页面的链表入口页面号。
组织结构如下:
tbsname.tbs
tbsname_v0/tbsname1
		  tbsname2
		  ...
		  tbsname199

tbsname_v1/tbsname200
		  tbsname201
		  ...
		  tbsname399

第一个文件位于不带序号，位于表空间根目录，其他文件按照200个一个子目录(卷)存放。
每个文件的页面数是相同的，通过页面号可以计算出页面位于哪个文件中。
*/
#pragma once

#include <string>
#include <assert.h>
#include <stdint.h>
#include "ec_diskio.h"
#include "ec_file.h"
#include "ec_log.h"
#include "ec_stream.h"
#include "ec_crc.h"

#ifndef TBS_OPEN_FILES
#define TBS_OPEN_FILES 8 // > 2 同时打开的文件数, LRU(Least Recently Used)队列大小
#endif
namespace ec {
	using size_tbs = int64_t;
	constexpr uint32_t TBS_MAGIC = 0x9ad21e21;//table space magic number
	constexpr uint32_t TBS_VERSION = 0x10000; //table space version
	constexpr int32_t TBS_DYNA_POS = 4096; //dynamic info start position
	constexpr int TBS_VOL_FILES = 200;//number files per volume(Subdirectory)
	constexpr size_tbs TBS_KILO = 1024; // Kilo
	constexpr int TBS_HEADPAGESIZE = 8192;
	constexpr int TBS_PARAM_SIZE = 128;
	constexpr int TBS_INFO_SIZE = 128;
	constexpr uint32_t TBS_FREEPAGE_MAGIC = 0xf1f2f3f4;
	constexpr int TBS_PGHEAD_SIZE = 24; // free page head info size
	constexpr const char* TBS_VOL_STR = "_v";
	enum tbs_error {
		tbs_ok = 0,
		tbs_err_failed = 1,
		tbs_err_exist = 2,
		tbs_err_isopen = 3,
		tbs_err_param = 4,
		tbs_err_createdir = 5,
		tbs_err_createfile = 6,
		tbs_err_openfile = 7,
		tbs_err_read = 8,
		tbs_err_write = 9,
		tbs_err_seek = 10,
		tbs_err_headcheck = 11,
		tbs_err_volerr = 12,
		tbs_err_name = 13,
		tbs_err_full = 14,
		tbs_err_pghead = 15,
		tbs_err_overflow = 16
	};

	/**
	 * @brief 表空间静态信息，创建后不再改变，位于每个文件的开头128字节，按照小头格式序列化。
	*/
	class tbs_param //
	{
	public:
		//static info
		uint32_t _magic; // TBS_MAGIC
		uint32_t _verson;// TBS_VERSION
		char     _tbsname[16]; //application magic
		int32_t  _fileno; // file No. 0->n
		int32_t  _pagekiolsize; //number of Kbytes,[1,32] default 8,  (8 * 1024)bytes
		int32_t  _filekiolpages;//number Kilo-pages per file. default 256, (256 * 1024)  = 262144
		int32_t  _maxfiles; //max number of files, default is 16384 (8192 * 262144 * 16384 = 8K * 256K *16K = 32768GB = 32TB)
		uint8_t  _res[84];
		uint32_t _crc32;
		//sizeof = 128

		tbs_param()
			: _magic(TBS_MAGIC)
			, _verson(TBS_VERSION)
			, _pagekiolsize(8)
			, _filekiolpages(256)
			, _maxfiles(16384)
			, _crc32(0)
		{
			memset(_tbsname, 0, sizeof(_tbsname));
			memset(_res, 0, sizeof(_res));
		}

		// serialize to buffer, return size , 0: error
		size_t serialize(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			try {
				ss << _magic << _verson;
				ss.write(_tbsname, sizeof(_tbsname));
				ss << _fileno << _pagekiolsize << _filekiolpages << _maxfiles;
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
				ss >> _magic >> _verson;
				ss.read(_tbsname, sizeof(_tbsname));
				ss >> _fileno >> _pagekiolsize >> _filekiolpages >> _maxfiles;
				ss.read(_res, sizeof(_res));
				uc = ec::crc32(pout, (uint32_t)ss.getpos());
				ss >> _crc32;
				if (uc != _crc32 || _magic != TBS_MAGIC)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	/**
	 * @brief 空闲页面头信息，位于页面开头24字节，使用小头格式序列化。空闲页面使用单向连接模式
	*/
	class tbs_freepagehead
	{
	public:
		//static info
		uint32_t _magic; // TBS_FREEPAGE_MAGIC = 0xf1f2f3f4
		uint32_t _verson; // TBS_VERSION
		size_tbs _pgnonext; // next free page no; -1: no next free page
		uint32_t _ures;
		uint32_t _crc32;
		//sizeof = 24

		tbs_freepagehead()
			: _magic(TBS_FREEPAGE_MAGIC)
			, _verson(TBS_VERSION)
			, _pgnonext(-1)
			, _ures(0)
			, _crc32(0)
		{
		}

		void reset() {
			_magic = TBS_FREEPAGE_MAGIC;
			_verson = TBS_VERSION;
			_pgnonext = -1;
			_ures = 0;
			_crc32 = 0;
		}

		// serialize to buffer, return size , 0: error
		size_t serialize(void* pout, size_t sizeout)
		{
			ec::stream ss(pout, sizeout);
			try {
				ss << _magic << _verson << _pgnonext << _ures;
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
				ss >> _magic >> _verson >> _pgnonext >> _ures;
				uc = ec::crc32(pout, (uint32_t)ss.getpos());
				ss >> _crc32;
				if (uc != _crc32 || _magic != TBS_FREEPAGE_MAGIC)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	//table space dynamic information, save in TBS_DYNA_POS offset of tbs first file

	/**
	 * @brief 表空间动态信息，位于第一个文件的 TBS_DYNA_POS 开始 128字节，保存了3个信息，使用小头格式序列化。
	*/
	class tbs_info
	{
	public:
		//dynamic info
		uint32_t _magic; // TBS_MAGIC
		uint32_t _verson;// TBS_VERSION
		size_tbs _numallpages;// number of all pages, grown 1024 pages once
		size_tbs _nextpageno; // next free page No, -1: no free page
		size_tbs _numfreepages;// number of free pages
		uint8_t  _res[92];
		uint32_t _crc32;
		//sizeof = 128

		tbs_info()
			: _magic(TBS_MAGIC)
			, _verson(TBS_VERSION)
			, _numallpages(0)
			, _nextpageno(-1)
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
				if (uc != _crc32 || _magic != TBS_MAGIC)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	/**
	 * @brief 表空间，包含了表空间的创建，打开；页面的分配，释放，读，些方法
	*/
	class tablespace
	{
	public:
		struct t_file
		{
			ec::File* pfile;
			t_file* pprev;
			t_file* pnext;
			int _key; // 文件号作为索引
		};		

		/**
		 * @brief 打开的文件管理，活跃的放在头部。
		*/
		class files_
		{		
		protected:
			t_file* _phead;
			ec::File* _file0; // 第一个文件，一直存在，不被释放
			
			int _ntop;//指向空, =0 表示没有空间了。
			int _nres;
			t_file _nodebuf[TBS_OPEN_FILES];//固定大小

			t_file* mallocbuf()
			{
				if (!_ntop)
					return nullptr;
				return &_nodebuf[--_ntop];
			}
		public:
			files_() :_phead(nullptr), _file0(nullptr) {
				for (auto i = 0; i < TBS_OPEN_FILES; i++) {
					_nodebuf[i].pfile = nullptr;
					_nodebuf[i].pnext = nullptr;
					_nodebuf[i].pprev = nullptr;
					_nodebuf[i]._key = -1;
				}
				_ntop = TBS_OPEN_FILES; //满空间可用
				_nres = 0;
			}

			virtual ~files_()
			{
				Close();
			}

			/**
			 * @brief 获取文件，存在则移动到链表头部。
			 * @param key 
			 * @return 存在返回文件对象，不存在返回nullptr
			*/
			ec::File* get(int key)
			{
				if (!key)
					return _file0;
				t_file* p = _phead;
				while (p) {
					if (p->_key == key) {
						if (p->pprev) {
							p->pprev->pnext = p->pnext;
							if(p->pnext)
								p->pnext->pprev = p->pprev;
							_phead->pprev = p;
							p->pnext = _phead;
							p->pprev = nullptr;
							_phead = p;
						}
						return p->pfile;
					}
					p = p->pnext;
				}
				return nullptr;
			}

			/**
			 * @brief 添加文件对象到头部，如果缓存满，则删除尾部的文件对象
			 * @param key 文件号
			 * @param pfile 文件对象
			 * @return 0: success; -1: failed;
			*/
			int add(int key, ec::File* pfile)
			{
				if (!key) { //头文件
					if (_file0 == pfile)
						return 0;
					if (_file0)
						delete _file0;
					_file0 = pfile;
					return 0;
				}
				t_file* p = mallocbuf();
				if (p) { //不满直接放在前面
					p->_key = key;
					p->pfile = pfile;
					if (_phead)
						_phead->pprev = p;
					p->pnext = _phead;
					p->pprev = nullptr;
					_phead = p;
					return 0;
				}
				p = _phead;
				while (p && p->pnext) // to tail
					p = p->pnext;
				if (!p || !p->pprev)
					return -1;

				delete p->pfile; //删除最后一个文件，存储新文件。
				p->pfile = pfile;
				p->_key = key;

				p->pprev->pnext = nullptr; //移动到头部
				_phead->pprev = p;
				p->pnext = _phead;
				p->pprev = nullptr;
				_phead = p;
				return 0;
			}

			/**
			 * @brief 关闭所有打开的文件
			*/
			void Close()
			{
				t_file* p = _phead;
				while (p) {
					if (p->pfile) {
						delete p->pfile;
						p->pfile = nullptr;
					}
					p = p->pnext;
				}
				_phead = nullptr;
				if (_file0) {
					delete _file0;
					_file0 = nullptr;
				}
				_ntop = TBS_OPEN_FILES;
			}
		};
	protected:
		int _lasterr;
		ec::ilog* _plog;
		std::string _spath;//table space director
		std::string _sname;//table space name, less 8 chars

		tbs_param _args; //静态信息
		tbs_info _info;  //动态信息
		files_ _files; // 打开的文件句柄缓冲,LRU队列
	public:
		tablespace(ec::ilog* plog = nullptr)
			: _lasterr(0)
			, _plog(plog)
		{
		}
		inline int64_t sizeTabspace() {
			return _args._pagekiolsize * TBS_KILO * _info._numallpages;
		}
		void setlog(ec::ilog* plog) {
			_plog = plog;
		}

		inline ec::ilog* getlog()
		{
			return _plog;
		}

		int getlasterr()
		{
			return _lasterr;
		}

		inline bool isopen() {
			return nullptr != _files.get(0);
		}

		inline size_tbs NumAllPages() {
			return _info._numallpages;
		}

		inline size_tbs NumFreePages() {
			return _info._numfreepages;
		}

		/**
		 * @brief 创建一个空的表空间，如果已存在会返回失败，错误码置为tbs_err_isopen
		 * @param spathutf8 表空间目录
		 * @param snameutf8 表空间名
		 * @param pagekiolsize 页面大小，单位kbyte(1024),取值 [1,32]表示 1024* 1 -> 32 * 1024字节
		 * @param filekiolpages 每个文件页面数，单位k(1024),取值 [1,1024], 表示 1024 -> 1024 * 1024个页面
		 * @param maxfiles 最大文件数，取值 1 -> 1024 * 1024。
		 * @return 0:ok; -1:failed
		 * @remark 当表空间已经打开时会返回失败，错误码置tbs_err_isopen, 因此如果要重新创建新的表空间，要先关闭；pagekiolsize,filekiolpages,maxfiles创建后不能修改。
		*/
		int Create(const char* spathutf8, const char* snameutf8, int pagekiolsize, int filekiolpages, int maxfiles)
		{
			if (isopen()) {
				_lasterr = tbs_err_isopen;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "create %s error(%d), table space is open", snameutf8, _lasterr);
				return -1;
			}
			assert(pagekiolsize <= 32 && pagekiolsize > 0 && filekiolpages > 0 && filekiolpages <= 1024 && maxfiles > 0);

			_spath = spathutf8;
			ec::formatpath(_spath);
			if (!ec::io::createdir(_spath.c_str())) {
				_lasterr = tbs_err_createdir;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "createdir %s error(%d) at tablespace::create. system errno %d", _spath.c_str(), _lasterr, SysIoErr());
				return -1;
			}
			_sname = snameutf8;
			std::string sfile;
			sfile.append(_spath.c_str()).append(_sname.c_str()).append(".tbs");
			if (ec::io::exist(sfile.c_str())) {
				_lasterr = tbs_err_exist;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "table space file %s is exist at tablespace::create. error(%d)",
						sfile.c_str(), _lasterr);
				}
				return -1;
			}

			ec::File* pfile = new ec::File;
			if (!pfile->Open(sfile.c_str(), File::OF_CREAT | File::OF_RDWR | File::OF_SYNC, File::OF_SHARE_READ)) { //创建
				_lasterr = tbs_err_createfile;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "create table space file %s error(%d). system errno %d", sfile.c_str(), _lasterr, SysIoErr());
				delete pfile;
				return -1;
			}
			ec::utf8_strlcpy(_args._tbsname, snameutf8, sizeof(_args._tbsname));

			_args._pagekiolsize = pagekiolsize;
			_args._filekiolpages = filekiolpages;
			_args._maxfiles = maxfiles;

			char pg[TBS_HEADPAGESIZE];
			memset(pg, 0, sizeof(pg));
			_args.serialize(pg, TBS_DYNA_POS);
			_info.serialize(&pg[TBS_DYNA_POS], TBS_HEADPAGESIZE - TBS_DYNA_POS);

			if (TBS_HEADPAGESIZE != pfile->Write(pg, TBS_HEADPAGESIZE)) {
				_lasterr = tbs_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "write table space file %s head error(%d). system errno %d", sfile.c_str(), _lasterr, SysIoErr());
				delete pfile;
				return -1;
			}
			pfile->flush();
			_files.add(0, pfile);//加入文件句柄缓冲
			_lasterr = tbs_ok;

			if (_plog) {
				_plog->add(CLOG_DEFAULT_INF, "Create TableSpace %s sizePerPage=%dKiB, sizePerFile=%dMiB", snameutf8, _args._pagekiolsize, _args._filekiolpages);
			}
			return 0;
		}

		/**
		 * @brief 打开表空间，存在会返回失败
		 * @param spathutf8 
		 * @param snameutf8 
		 * @return -1:failed; 0：success
		 * @remark 当表空间已经打开时会返回失败，错误码置tbs_err_isopen, 因此如果要重新打开新的表空间，要先关闭；
		 * 一旦创建后，表空间名称不能变(作为应用标识)，目录可以变，可以移动拷贝，不能改名。
		*/
		int Open(const char* spathutf8, const char* snameutf8)
		{
			if (isopen()) {
				_lasterr = tbs_err_isopen;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "open %s error(%d), table space is open", snameutf8, _lasterr);
				return -1;
			}
			_spath = spathutf8;
			ec::formatpath(_spath);
			_sname = snameutf8;
			std::string sfile;
			sfile.append(_spath.c_str()).append(_sname.c_str()).append(".tbs");
			ec::File* pfile = new ec::File;
			if (!pfile->Open(sfile.c_str(), File::OF_RDWR | File::OF_SYNC, File::OF_SHARE_READ)) { //打开头文件，共享读模式
				_lasterr = tbs_err_openfile;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "open table space file %s error(%d). system errno %d", sfile.c_str(), _lasterr, SysIoErr());
				delete pfile;
				return -1;
			}
			char pg[TBS_HEADPAGESIZE]; //文件头8K区
			memset(pg, 0, sizeof(pg));

			if (pfile->Read(pg, TBS_HEADPAGESIZE) < 0) {
				_lasterr = tbs_err_read;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "table space %s read %s head error(%d). system errno %d",
						_sname.c_str(), sfile.c_str(), _lasterr, SysIoErr());
				}
				delete pfile;
				return -1;
			}

			if (_args.parse(pg, TBS_PARAM_SIZE) < 0) { //解析静态信息
				_lasterr = tbs_err_headcheck;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "table space %s read %s headcheck error(%d).",
						_sname.c_str(), sfile.c_str(), _lasterr);
				}
				delete pfile;
				return -1;
			}
			if (_args._fileno != 0 || !ec::streq(_args._tbsname, snameutf8)) { //检查表空间名称是否一致
				_lasterr = tbs_err_volerr;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "table space %s file %s head parameter error %d. fileno= 0/%d, tbsname=%s/%s",
						_sname.c_str(), sfile.c_str(), _lasterr, _args._fileno, _sname.c_str(), _args._tbsname);
				}
				delete pfile;
				return -1;
			}
			if (_plog) {
				_plog->add(CLOG_DEFAULT_INF, "Open TableSpace %s sizePerPage=%dKiB, sizePerFile=%dMiB", snameutf8, _args._pagekiolsize, _args._filekiolpages);
			}
			//读info
			if (_info.parse(&pg[TBS_DYNA_POS], TBS_INFO_SIZE) < 0) { //读取动态信息
				_lasterr = tbs_err_volerr;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "table space %s file %s page info parse error(%d).",
						_sname.c_str(), sfile.c_str(), _lasterr);
				}
				delete pfile;
				return -1;
			}
			_files.add(0, pfile);
			_lasterr = tbs_ok;
			return 0;
		}

		static bool isExist(const char* spathutf8, const char* snameutf8)
		{
			std::string spath = spathutf8;
			ec::formatpath(spath);
			spath.append(snameutf8).append(".tbs");
			return ec::io::exist(spath.c_str());
		}

		inline size_tbs pagesize() // page bytes size
		{
			return _args._pagekiolsize * TBS_KILO;
		}

		inline size_t SizePage() // page bytes size
		{
			return _args._pagekiolsize * TBS_KILO;
		}

		inline size_tbs filepages() // number of pages per file
		{
			return _args._filekiolpages * TBS_KILO;
		}

		/**
		 * @brief 分配页面,页面不够时会自动增长。
		 * @return -1:failed; >=0 分配的页面号
		 * @remark 分配一个页面最少为两次磁盘写入，遇到增长空间时，为TBS_KILO/4次写入。分配的页面头被清零消耗一次磁盘写入换来防止重复写入和扫描回收失联页面，
		*/
		size_tbs pagealloc()
		{
			int n = 1; //容许重新增长一次
			if (_info._nextpageno == -1) { //没有空闲页面
				if (_args._maxfiles && _info._numallpages / filepages() >= _args._maxfiles) {
					_lasterr = tbs_err_full;
					return -1; //超过表空间上限
				}
				if (grownpages() < 0)
					return -1;
				--n;
			}
			while (n >= 0) {
				int nfileno = static_cast<int>(_info._nextpageno / filepages());
				size_tbs filepos = TBS_HEADPAGESIZE + (_info._nextpageno % filepages()) * pagesize(); //计算相对该文件头位置
				ec::File* pfile = _files.get(nfileno);
				if (!pfile) { //不存在则打开页面文件
					if (!nfileno) {
						_lasterr = tbs_err_failed;
						if (_plog)
							_plog->add(CLOG_DEFAULT_ERR, "alloc page table space %s failed. file0 == nullptr", _sname.c_str());
						return -1;
					}
					if (nullptr == (pfile = openpagefile(nfileno)))
						return -1;	//有空闲页面只考虑打开文件，不考虑创建，在页面增长时已经创建好了文件。
				}

				uint8_t headbuf[TBS_PGHEAD_SIZE]; //读取空闲页面头部信息
				if (TBS_PGHEAD_SIZE != pfile->ReadFrom(filepos, headbuf, TBS_PGHEAD_SIZE)) {
					_lasterr = tbs_err_pghead;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "table space %s fileno %d alloc page %jd read head error(%d). system errno %d",
							_sname.c_str(), nfileno, _info._nextpageno, _lasterr, SysIoErr());
					}
					--n;
					if (n >= 0 && grownpages() < 0) { //这里增加容错, 丢弃空闲链表，重新增长页面一次
						return -1;
					}
					continue;
				}

				tbs_freepagehead pgh; //解析空闲页面头部信息
				if (pgh.parse(headbuf, TBS_PGHEAD_SIZE) < 0) {
					_lasterr = tbs_err_pghead;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "table space %s fileno %d alloc page %jd head parse error(%d).",
							_sname.c_str(), nfileno, _info._nextpageno, _lasterr);
					}
					--n;
					if (n >= 0 && grownpages() < 0) { //这里增加容错, 丢弃空闲链表，重新增长页面一次
						return -1;
					}
					continue;
				}
				size_tbs pgno = _info._nextpageno;
				_info._nextpageno = pgh._pgnonext; //更改动态表空间动态信息的下一个空闲页面的号
				_info._numfreepages -= 1; //更改动态表空间动态信息的空闲页面总数
				if (updateinfo() < 0) //更新到磁盘
					return -1;

				memset(headbuf, -1, sizeof(headbuf));//用0xFF填充,释放时读取判断防止重复释放，以后也可以使用扫描工具找回失联的页面。
				pfile->WriteTo(filepos, headbuf, TBS_PGHEAD_SIZE);
				return pgno;
			}
			return -1;
		}

		/**
		 * @brief 释放页面，支持防止重复释放.
		 * @param pgno 
		 * @return 0:success; -1:failed
		*/
		int pagefree(size_tbs pgno)
		{
			if (pgno >= _info._numallpages) { //超出范围
				_lasterr = tbs_err_failed;
				if (_plog) {
					_plog->add(CLOG_DEFAULT_ERR, "free page table space %s pageno(%jd) uplimit error(%d).",
						_sname.c_str(), pgno, _lasterr);
				}
				return -1;
			}
			int nfileno = static_cast<int>(pgno / filepages());
			size_tbs filepos = TBS_HEADPAGESIZE + (pgno % filepages()) * pagesize();
			ec::File* pfile = _files.get(nfileno);
			if (!pfile) {
				if (!nfileno) {
					_lasterr = tbs_err_failed;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "free page table space %s pageno(%jd) error(%d). file0 == nullptr",
							_sname.c_str(), pgno, _lasterr);
					}
					return -1;
				}
				if (nullptr == (pfile = openpagefile(nfileno)))
					return -1;
			}

			uint8_t headbuf[TBS_PGHEAD_SIZE] = { 0 };
			tbs_freepagehead pgh;//先读回来判断是否是空闲页面,避免重复释放
			pfile->ReadFrom(filepos, headbuf, TBS_PGHEAD_SIZE);
			if (0 == pgh.parse(headbuf, TBS_PGHEAD_SIZE)) {
				if (_plog)
					_plog->add(CLOG_DEFAULT_WRN, "table space %s fileno %d free page %jd refree error",
						_sname.c_str(), nfileno, pgno);
				return 0;
			}

			pgh.reset(); //写空闲页面头
			pgh._pgnonext = _info._nextpageno;
			pgh.serialize(headbuf, sizeof(headbuf));
			if (TBS_PGHEAD_SIZE != pfile->WriteTo(filepos, headbuf, TBS_PGHEAD_SIZE)) {
				_lasterr = tbs_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s fileno %d free page %jd head write error(%d), systen errno %d",
						_sname.c_str(), nfileno, pgno, _lasterr, SysIoErr());
				return -1;
			}
			_info._nextpageno = pgno;
			_info._numfreepages += 1;
			return updateinfo();
		}

		/**
		 * @brief 写页面, 如果会写出页面范围直接返回-1，错误码置tbs_err_overflow，并不会写入任何内容。
		 * @param pgno 页面号
		 * @param pgoff 页面内偏移
		 * @param pdata 数据
		 * @param size 数据字节数
		 * @return return 0:ok; -1:error
		*/
		int writepage(size_tbs pgno, size_t pgoff, const void* pdata, size_t size)
		{
			if (pgno < 0 || pgno >= _info._numallpages || pgoff + size >(size_t)pagesize()) {//检查参数合法性和是否会写出页面
				_lasterr = tbs_err_overflow;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s write page pgno=%jd,pgoff=%zu,size=%zu overflow error(%d).",
						_sname.c_str(), pgno, pgoff, size, _lasterr);
				return -1;
			}
			int nfileno = static_cast<int>(pgno / filepages());//定位文件号
			ec::File* pfile = _files.get(nfileno);
			if (!pfile) {
				if (!nfileno) {
					_lasterr = tbs_err_failed;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "table space %s write page pgno=%jd,pgoff=%zu,size=%zu error(%d) open fileno=%d failed.",
							_sname.c_str(), pgno, pgoff, size, _lasterr, nfileno);
					}
					return -1;
				}
				if (nullptr == (pfile = openpagefile(nfileno))) {//页面是预分配的，不需要再创建文件
					_lasterr = tbs_err_openfile;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "table space %s write page pgno=%jd,pgoff=%zu,size=%zu error(%d) open fileno=%d failed.",
							_sname.c_str(), pgno, pgoff, size, _lasterr, nfileno);
					}
					return -1;
				}
			}
			size_tbs filepos = TBS_HEADPAGESIZE + (pgno % filepages()) * pagesize() + (size_tbs)pgoff;//定位写入位置
			if (pfile->WriteTo(filepos, pdata, (uint32_t)size) < 0) {
				_lasterr = tbs_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s write page pgno=%jd,pgoff=%zu,size=%zu write error(%d). system errno %d",
						_sname.c_str(), pgno, pgoff, size, _lasterr, SysIoErr());
				return -1;
			}
			_lasterr = 0;
			return 0;
		}

		// return >=0: read size, may be less size; -1:error

		/**
		 * @brief 读页面, 如果会读出页面范围直接返回-1，错误码置tbs_err_overflow，并不会读回任何内容。
		 * @param pgno 页面号
		 * @param pgoff 页面内偏移
		 * @param pdata 数据
		 * @param size 数据字节数
		 * @return 返回读取的字节数，可能或小于size；如果发生错误，返回-1；
		*/
		int readpage(size_tbs pgno, size_t pgoff, void* pdata, size_t size)
		{
			if (pgno < 0 || pgno >= _info._numallpages || pgoff >= (size_t)pagesize()) {//判断参数合法性
				_lasterr = tbs_err_overflow;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s read page pgno=%jd/%jd,pgoff=%zu,size=%zu/%zu overflow error(%d).",
						_sname.c_str(), pgno, _info._numallpages, pgoff, size, (size_t)pagesize(), _lasterr);
				return -1;
			}
			int nfileno = static_cast<int>(pgno / filepages());//定位文件号
			ec::File* pfile = _files.get(nfileno);
			if (!pfile) {
				if (!nfileno) {
					_lasterr = tbs_err_failed;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "table space %s read page pgno=%jd,pgoff=%zu,size=%zu error(%d) open fileno=%d failed.",
							_sname.c_str(), pgno, pgoff, size, _lasterr, nfileno);
					}
					return -1;
				}
				if (nullptr == (pfile = openpagefile(nfileno))) {
					_lasterr = tbs_err_openfile;
					if (_plog) {
						_plog->add(CLOG_DEFAULT_ERR, "table space %s read page pgno=%jd,pgoff=%zu,size=%zu error(%d) open fileno=%d failed.",
							_sname.c_str(), pgno, pgoff, size, _lasterr, nfileno);
					}
					return -1;
				}
			}
			uint32_t ur = (uint32_t)size;
			if ((size_tbs)pgoff + ur > pagesize())
				ur = static_cast<uint32_t>(pagesize() - pgoff);
			size_tbs filepos = TBS_HEADPAGESIZE + (pgno % filepages()) * pagesize() + (size_tbs)pgoff;

			int nr = pfile->ReadFrom(filepos, pdata, ur);
			if (nr < 0) {
				_lasterr = tbs_err_read;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s read page pgno=%jd,pgoff=%zu,size=%zu read error(%d) system errno %d",
						_sname.c_str(), pgno, pgoff, size, _lasterr, SysIoErr());
				return -1;
			}
			_lasterr = 0;
			return nr;
		}

		int SysIoErr()
		{
#ifdef _WIN32
			return (int)GetLastError();
#else
			return errno;
#endif
		}
	protected:
		/**
		 * @brief 更新表空间动态信息，位于头文件 TBS_DYNA_POS 位置开始 TBS_INFO_SIZE 字节
		 * @return 
		*/
		int updateinfo()
		{
			uint8_t ubuf[TBS_INFO_SIZE];
			memset(ubuf, 0, sizeof(ubuf));
			_info.serialize(ubuf, sizeof(ubuf));
			ec::File* pfile = _files.get(0);
			if (!pfile) {
				_lasterr = tbs_err_failed;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s updateinfo failed. file0 = nullptr", _sname.c_str());
				return -1;
			}

			if (pfile->WriteTo(TBS_DYNA_POS, ubuf, TBS_INFO_SIZE) < 0) {
				_lasterr = tbs_err_write;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s  write page info error(%d) system errno %d.", _sname.c_str(), _lasterr, SysIoErr());
				return -1;
			}
			_lasterr = tbs_ok;
			if (_plog) {
				_plog->add(CLOG_DEFAULT_ALL, "update table space %s success, numpages=%jd, numfreepages=%jd, nextpgno=%jd",
					_sname.c_str(), _info._numallpages, _info._numfreepages, _info._nextpageno);
			}
			return 0;
		}

		/**
		 * @brief 批量增长空闲页面。
		 * @return 
		*/
		int grownpages()
		{
			int nfileno = static_cast<int>(_info._numallpages / filepages()); //计算出当前表空间最后的文件名。
			ec::File* pfile = _files.get(nfileno);
			std::string sfile = _spath;
			if (!nfileno) //头文件
				sfile.append(_sname.c_str()).append(".tbs");
			else
				sfile.append(_sname.c_str()).append(TBS_VOL_STR).append(std::to_string(nfileno / TBS_VOL_FILES)).append("/");
			if (!pfile) {//不存在
				if (!nfileno) { // 头卷没打开，不可能出现这种情况，返回错误。
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "grown table space %s failed. file0 == nullptr", _sname.c_str());
					_lasterr = tbs_err_failed;
					return -1;
				}
				sfile.append(_sname.c_str()).append(std::to_string(nfileno)); // "path/name_v1/name200" 非头文件没有".tbs"后缀
				if (ec::io::exist(sfile.c_str()))
					pfile = openpagefile(nfileno); //存在则打开非头文件。并加入文件句柄缓冲
				else
					pfile = createpagefile(nfileno);//不存在创建非头文件。并加入文件句柄缓冲
				if (!pfile)
					return -1;
			}
			return grownfilepages(nfileno, pfile, sfile.c_str()); //在当前文件中增加页面，增加TBS_KILO(1024)个或者直到文件满
		}

		/**
		 * @brief 在打开的文件中增长页面
		 * @param nfileno 文件号
		 * @param pfile 打开的文件
		 * @param sfile 文件名,用于日志输出。
		 * @return 0：succeed；-1：failed
		*/
		int grownfilepages(int nfileno, ec::File* pfile, const char* sfile) 
		{
			if (pfile->Seek(TBS_HEADPAGESIZE + (_info._numallpages % filepages()) * pagesize(), File::seek_set) < 0) {
				_lasterr = tbs_err_seek;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s grown %s failed. system errno %d", _sname.c_str(), sfile, SysIoErr());
				return -1;
			}
			char buf[1024 * 32]; // pagesize must <= 32K
			memset(buf, -1, sizeof(buf)); //先填写-1

			//增加 TBS_KILO / 4 页面，因为每个文件的页面数是 TBS_KILO的整数倍，所以不会写出写跨文件
			int ng = TBS_KILO / 4;
			tbs_freepagehead hd;
			for (auto i = 0; i < ng; i++) {
				if (i + 1 == ng)
					hd._pgnonext = _info._nextpageno; //最后一个页面，这里的_info._nextpageno一定是 -1
				else
					hd._pgnonext = _info._numallpages + i + 1;
				hd.serialize(buf, sizeof(buf));
				if (pfile->Write(buf, static_cast<uint32_t>(pagesize())) < 0) {
					_lasterr = tbs_err_write;
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "table space %s grown %s write page failed. system errno %d", _sname.c_str(), sfile, SysIoErr());
					return -1;
				}
			}
			_info._nextpageno = _info._numallpages; //最后改写表空间的动态信息
			_info._numallpages += ng;
			_info._numfreepages += ng;
			pfile->flush(); //文件内容刷新到磁盘
			return updateinfo();//动态信息更新到头文件。
		}

		/**
		 * @brief 创建非头文件, 会自动创建目录
		 * @param nfileno 文件号
		 * @return 返回文件类指针，nullptr表示失败。
		*/
		ec::File* createpagefile(int nfileno)
		{
			assert(nfileno != 0);
			std::string sfile = _spath;
			sfile.append(_sname.c_str()).append(TBS_VOL_STR).append(std::to_string(nfileno / TBS_VOL_FILES)).append("/");

			if (!ec::io::createdir(sfile.c_str())) { //先创建目录
				_lasterr = tbs_err_createdir;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s  createdir %s failed. system errno %d", _sname.c_str(), sfile.c_str(), SysIoErr());
				return nullptr;
			}

			sfile.append(_sname.c_str()).append(std::to_string(nfileno));
			ec::File* pfile = new ec::File;
			if (!pfile->Open(sfile.c_str(), File::OF_CREAT | File::OF_RDWR, File::OF_SHARE_READ)) {
				_lasterr = tbs_err_createfile;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s createfile %s failed. system errno %d",
						_sname.c_str(), sfile.c_str(), SysIoErr());
				delete pfile;
				return nullptr;
			}
			//写文件静态信息
			tbs_param param;
			param._fileno = nfileno;
			memcpy(param._tbsname, _args._tbsname, sizeof(param._tbsname));
			param._pagekiolsize = _args._pagekiolsize;
			param._filekiolpages = _args._filekiolpages;
			param._maxfiles = _args._maxfiles;

			char pg[TBS_HEADPAGESIZE]; //写满8K，未使用的填写0
			memset(pg, 0, sizeof(pg));
			param.serialize(pg, sizeof(pg));

			if (TBS_HEADPAGESIZE != pfile->Write(pg, TBS_HEADPAGESIZE)) {
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "write table space file %s head failed. system errno %d", sfile.c_str(), SysIoErr());
				_lasterr = tbs_err_write;
				delete pfile;
				return nullptr;
			}
			_lasterr = tbs_ok;
			_files.add(nfileno, pfile);
			return pfile;
		}

		/**
		 * @brief 打开非头文件。并加入文件句柄缓冲
		 * @param nfileno 
		 * @return 返回文件对象，nullptr表示失败。
		*/
		ec::File* openpagefile(int nfileno)
		{
			assert(nfileno != 0);
			std::string sfile = _spath;
			sfile.append(_sname.c_str()).append(TBS_VOL_STR).append(std::to_string(nfileno / TBS_VOL_FILES)).append("/");
			sfile.append(_sname.c_str()).append(std::to_string(nfileno));

			ec::File* pfile = new ec::File;
			if (!pfile->Open(sfile.c_str(), File::OF_RDWR, File::OF_SHARE_READ)) {
				int nsyserr = SysIoErr();
				_lasterr = tbs_err_openfile;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s openfile %s failed. System errno %d",
						_sname.c_str(), sfile.c_str(), nsyserr);
				delete pfile;
				return nullptr;
			}
			//读文件信息
			tbs_param param;
			char pg[TBS_PARAM_SIZE];
			if (pfile->Read(pg, TBS_PARAM_SIZE) < 0) {
				_lasterr = tbs_err_read;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s read %s head failed. system errno %d ", _sname.c_str(), sfile.c_str(), SysIoErr());
				delete pfile;
				return nullptr;
			}
			if (param.parse(pg, TBS_PARAM_SIZE) < 0) {
				_lasterr = tbs_err_headcheck;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s file %s headcheck failed.", _sname.c_str(), sfile.c_str());
				delete pfile;
				return nullptr;
			}

			if (param._fileno != nfileno || !ec::streq(param._tbsname, _args._tbsname)) {
				_lasterr = tbs_err_volerr;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "table space %s file %s head info error. fileno=%d/%d, tbsname=%s/%s",
						_sname.c_str(), sfile.c_str(), nfileno, param._fileno, _sname.c_str(), param._tbsname);
				delete pfile;
				return nullptr;
			}
			_lasterr = tbs_ok;
			_files.add(nfileno, pfile);
			return pfile;
		}
	};
}//namespace ec
