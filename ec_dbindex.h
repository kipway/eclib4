/*!
\file ec_dbindex.h
数据表索引

使用一个磁盘内存同步哈希表ec::objfile存储数据表的索引入口对象(包括表名,B+树root页面号,索引页面数,可选的uint32的短ID)。
使用一个表空间ec::tablespace存储所有数据表的索引。

\author jiangyong

\update 
  2025.6.19  索引入口由ec::recfile改为 ec::objfile
  2025.6.18  索引表空间页面大小由宏改为Open时参数指定。
  2024-2-1   clear include. Use ec::fixstring instead of ec::array<char>
  2022-12-14 增加索引表空间大小获取

*/
#pragma once

#include "ec_map.h"
#include "ec_tbs.h"
#include "ec_objfile.h"
#include "ec_protoc.h"
#include "ec_ipgstorage.h"
#include "ec_bptree.h"

#ifndef DB_IDXOBF_PAGESIZE  //一个索引入口对象文件页面大小
#define DB_IDXOBF_PAGESIZE 512
#endif

#ifndef DB_IDXOBF_APPFLAG
#define DB_IDXOBF_APPFLAG  "tagidxobf"
#endif

#ifndef DB_IDXOBF_HASHSIZE //索引标签表hash尺寸
#define DB_IDXOBF_HASHSIZE 16384
#endif

#ifndef DB_INDEX_TBS_FILEKIOLPAGES //索引表空间的单个文件页面KIOL数,
#define DB_INDEX_TBS_FILEKIOLPAGES 128 //  128 * 1024 * 8192 = 1G
#endif

namespace ec
{
	/**
	 * @brief 一个数据表索引入口记录信息，被编码后存储在一个obf文件中
	*/
	class CTableIndexItem
	{
	public:
		enum {
			id_name = 1,
			id_recpos = 2,
			id_numidx = 3,
			id_rootindxpgno = 4,
			id_tagid = 5,
			id_rootdatapgno = 6
		};
		uint32_t _recpos;//rdf文件中的记录位置, -1标识没有存盘
		uint32_t _numidx;//索引的数据页面数,添加和删除时同步更新。
		int64_t _rootindxpgno; //索引表空间种的索引入口页面,-1标识没有索引
		int64_t _rootdatapgno; //数据表空间的第一个数据页面号,-1标识没有数据页面
		uint32_t _tagid; //和_name对应的tagid,唯一,存储在页面索引页面头部
		ec::string _name; //标签名,utf8编码
	public:
		CTableIndexItem()
			: _recpos(-1)
			, _numidx(0)
			, _rootindxpgno(-1)
			, _rootdatapgno(-1)
			, _tagid(0)
		{
		}
		
		void clear() //清除并设置默认值,在ec::pb::parse里被调用
		{
			_recpos = 0;
			_numidx = 0;
			_rootindxpgno = 0;
			_rootdatapgno = 0;
			_name.clear();
		}

		size_t size_content()
		{
			return ec::pb::size_var(id_tagid, _tagid)
				+ ec::pb::size_var(id_recpos, _recpos)
				+ ec::pb::size_var(id_numidx, _numidx)
				+ ec::pb::size_var(id_rootindxpgno, _rootindxpgno)
				+ ec::pb::size_var(id_rootdatapgno, _rootdatapgno)
				+ ec::pb::size_cls(id_name, _name.c_str(), _name.size());
		}

		template<class _Out>
		bool out_content(_Out* pout)
		{
			return ec::pb::out_var(pout, id_tagid, _tagid)
				&& ec::pb::out_var(pout, id_recpos, _recpos)
				&& ec::pb::out_var(pout, id_numidx, _numidx)
				&& ec::pb::out_var(pout, id_rootindxpgno, _rootindxpgno)
				&& ec::pb::out_var(pout, id_rootdatapgno, _rootdatapgno)
				&& ec::pb::out_cls(pout, id_name, _name.c_str(), _name.size());
		}

		void on_var(uint32_t field_number, uint64_t val) 
		{
			switch (field_number)
			{
				CASE_I32(id_recpos, _recpos);
				CASE_U32(id_numidx, _numidx);
				CASE_I64(id_rootindxpgno, _rootindxpgno);
				CASE_I64(id_rootdatapgno, _rootdatapgno);
				CASE_U32(id_tagid, _tagid);
			}
		}
		void on_fixed(uint32_t field_number, const void* pval, size_t size) {
		}
		void on_cls(uint32_t field_number, const void* pdata, size_t size)
		{
			switch (field_number)
			{
			case id_name:
				_name.assign((const char*)pdata, size);
				break;
			}
		}		
	};// class CIndexItem

	/**
	 * @brief 使用 ec::tablespace表空间存储的索引存储。
	*/
	class CIdxPgStorge : public ec::ipage_storage
	{
	public:
		CIdxPgStorge(ec::tablespace* ptbs) : _ptbs(ptbs){

		}
	protected:
		ec::tablespace* _ptbs; //存储表空间

	public:
		virtual size_t pg_size() //返回页面大小
		{
			return _ptbs->pagesize();
		}
		virtual int64_t pg_alloc() {
			return _ptbs->pagealloc();
		}// 分配一新的页面,返回页面号,-1表示失败

		virtual bool pg_free(int64_t pgno) {
			return 0 == _ptbs->pagefree(pgno);
		}// 删除页面

		virtual int  pg_read(int64_t pgno, size_t offset, void* pbuf, size_t bufsize) {
			return _ptbs->readpage(pgno, offset, pbuf, bufsize);
		}// 读页面，返回读取到的字节数，-1表示失败

		virtual int  pg_write(int64_t pgno, size_t offset, const void* pdata, size_t datasize) {
			return _ptbs->writepage(pgno, offset, pdata, datasize) ? -1 : (int)datasize;
		}// 写页面, 返回写入字节数; -1表示失败

	};// class CIdxPgStorge

	/**
	 * @brief 数据索引,多个标签(多个tree)的索引存储；包含两部分：入口信息和索引存储。
	 * 
	*/
	class CDataIndex
	{
	protected:
		ec::ilog* _plog; //日志输出
		ec::objfile _obf; //索引信息记录文件,存储每个标签的入口信息，常驻内存
		ec::tablespace _tbs;//索引表空间,存储具体的索引页面。
	public:
		struct keq_indexitem
		{
			bool operator()(const char* key, const CTableIndexItem& val)
			{
				return ec::strieq(key, val._name.c_str()); //不分大小写
			}
		};

		inline int64_t sizeTabspace() {
			return _tbs.sizeTabspace();
		}
	protected:

		/**
		 * @brief 常驻内存的索引入口hashmap，使用标签名索引，不分大小写
		*/
		ec::hashmap<const char*, CTableIndexItem, keq_indexitem, ec::del_mapnode<CTableIndexItem>, ec::hash_istr> _map;

	public:
		CDataIndex(ec::ilog* plog = nullptr) : _plog(plog), _obf(plog), _tbs(plog), _map(DB_IDXOBF_HASHSIZE)
		{
		}

		void SetLog(ec::ilog* plog)
		{
			_plog = plog;
			_obf.setLog(plog);
			_tbs.setlog(plog);
		}

		/**
		 * @brief 打开索引，不存在则创建
		 * @param nameobf 索引入口对象文件名，全路径，所在目录必须存在，一般在表空间目录下。
		 * @param pathtbs 索引表空间目录，可以不存在，会自动创建。
		 * @param nametbs 索引表空间名
		 * @param pagekiolsize 表控件页面千字节，单位 1024,填写16 表示页面大侠 16 * 1024
		 * @return 0:ok; -1:error
		*/
		int Open(const char* nameobf, const char* pathtbs, const char* nametbs, int pagekiolsize)
		{
			if (OpenObf(nameobf) < 0)
				return -1;
			return OpenTbs(pathtbs, nametbs, pagekiolsize);
		}

		/**
		 * @brief 读取一个标签索引值的数据页面号
		 * @param tagname 标签名
		 * @param idxval  索引值，比如timestamp
		 * @param ixout 输出索引值,<= idxval 
		 * @param ivout 输出对应的数据页面号
		 * @return 0: ok; -1: error
		*/
		int GetIdx(const char* tagname, int64_t idxval, int64_t* ixout, int64_t* ivout)
		{
			using clstree = ec::btree<>;
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return -1;
			CIdxPgStorge storge(&_tbs);
			clstree cls(&storge, pidx->_rootindxpgno);
			return cls.find(idxval, ixout, ivout) ? -1 : 0;
		}

		/**
		 * @brief 插入一个数据页面索引
		 * @param tagname 标签名
		 * @param idxval  索引值
		 * @param pgno  页面号
		 * @param tagid 标签uid
		 * @return 0: success; -1:error
		*/
		int InsertIdx(const char* tagname, int64_t idxval, int64_t pgno, uint32_t tagid, ec::ilog* plog = nullptr)
		{
			using clstree = ec::btree<>;
			int64_t rootpgno = -1;
			CTableIndexItem* pidx = _map.get(tagname);
			if (pidx)
				rootpgno = pidx->_rootindxpgno;

			CIdxPgStorge storge(&_tbs);
			clstree idxtree(&storge, rootpgno);
			int nrst = 0;
			if (idxtree.insert(idxval, pgno, nrst, plog) < 0)
				return -1;
			if (pidx) {
				if (BPTREE_ITEM_INSERTED == nrst || pidx->_rootindxpgno != idxtree.get_rootpgno()) {
					pidx->_rootindxpgno = idxtree.get_rootpgno();
					if (BPTREE_ITEM_INSERTED == nrst)
						pidx->_numidx += 1;
					if (pidx->_rootdatapgno < 0 || idxval == 0)
						pidx->_rootdatapgno = pgno;
					if (writeidx2obf(pidx) < 0) {
						pidx->_rootindxpgno = rootpgno;
						if (pidx->_numidx)
							pidx->_numidx -= 1;
						return -1;
					}
				}
				return 0;
			}
			CTableIndexItem idx;//新标签
			idx._name.assign(tagname);
			idx._numidx = 1;
			idx._rootindxpgno = idxtree.get_rootpgno();
			idx._recpos = -1;
			idx._tagid = tagid;
			idx._rootdatapgno = pgno; //新标签,第一个页面的索引的页面
			if (writeidx2obf(&idx) < 0)
				return -1;
			_map.set(tagname, std::move(idx));
			return 0;
		}

		/**
		 * @brief 删除一个数据索引记录
		 * @param tagname 标签名
		 * @param idxval 索引值
		 * @param pgno 数据页面号
		 * @return  0:success; -1:error
		*/
		int DelIdxRec(const char* tagname, int64_t idxval, int64_t pgno)
		{
			using clstree = ec::btree<>;
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return 0;
			CIdxPgStorge storge(&_tbs);
			clstree idxtree(&storge, pidx->_rootindxpgno);
			if (idxtree.erease(idxval, pgno) < 0)
				return -1;
			pidx->_rootindxpgno = idxtree.get_rootpgno();
			if (pidx->_numidx)
				pidx->_numidx -= 1;
			return writeidx2obf(pidx);
		}

		//清除指定name的对象全部索引，用于删除标签，fun为释放回收数据页面

		/**
		 * @brief 清除指定name的对象全部索引，用于删除标签，并从索引入口rdf文件中删除入口
		 * @param tagname 标签名
		 * @param fun 删除数据页面回调
		 * @return 0
		*/
		int ClearIdxTree(const char* tagname, std::function<void(int64_t idxv, int64_t idxpgno)> fun)
		{
			using clstree = ec::btree<>;
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return 0;
			if (pidx->_rootindxpgno >= 0) {
				CIdxPgStorge storge(&_tbs); //删除索引
				clstree idxtree(&storge, pidx->_rootindxpgno);
				idxtree.clear(fun);
			}
			if (pidx->_recpos >= 0)
				_obf.freeObject(pidx->_recpos);//删除磁盘记录
			_map.erase(tagname);//从内存map删除
			return 0;
		}

		/**
		 * @brief 遍历所有数据索引
		 * @param tagname 标签名
		 * @param fun 回调函数
		 * @return 0:success; -1:无此标签
		*/
		int ForEachDataIdx(const char* tagname, std::function<void(int64_t idxv, int64_t idxpgno)> fun)
		{
			using clstree = ec::btree<>;
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return -1;
			if (pidx->_rootindxpgno >= 0) {
				CIdxPgStorge storge(&_tbs); //删除索引
				clstree idxtree(&storge, pidx->_rootindxpgno);
				idxtree.foreach(fun);
			}
			return 0;
		}

		//读取一个标签的第一个数据页面号, 返回-1表示失败, >0为数据页面号
		int64_t GetRootDataPgNo(const char* tagname)
		{
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return -1;
			return pidx->_rootdatapgno;
		}

		//读取一个标签的索引数
		uint32_t GetIdxNum(const char* tagname)
		{
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return 0;
			return pidx->_numidx;
		}

		//索引计数减少一个,暂未使用
		int reduce(const char* tagname) {
			CTableIndexItem* pidx = _map.get(tagname);
			if (!pidx)
				return -1;
			if (!pidx->_numidx)
				return 0;
			pidx->_numidx -= 1;
			return writeidx2obf(pidx);
		}
	protected:

		/**
		 * @brief 打开索引入口记录文件，不存在则创建
		 * @param sfile 全路径索引入口记录文件名
		 * @return 0：success； -1:failed
		*/
		int OpenObf(const char* sfile)
		{
			int bcreate = 1;
			_plog->add(CLOG_DEFAULT_ALL, "start open recfile %s.", sfile);
			if (ec::io::exist(sfile)) {
				if (_obf.openFile(sfile, DB_IDXOBF_APPFLAG, true) < 0) {
					_plog->add(CLOG_DEFAULT_MSG, "objfile %s open failed!", sfile);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_ALL, "objfile %s open success!", sfile);
				bcreate = 0;
			}
			else if (_obf.createFile(sfile, DB_IDXOBF_APPFLAG, DB_IDXOBF_PAGESIZE, true) < 0) {
				_plog->add(CLOG_DEFAULT_MSG, "objfile %s is not exist, create failed!", sfile);
				return -1;
			}
			if (bcreate) {
				_plog->add(CLOG_DEFAULT_MSG, "objfile %s is not exist, create it success!", sfile);
			}
			int n = _obf.loadAll([&](void* prec, size_t size, uint32_t pos) {
				CTableIndexItem tv;
				if (!size)
					return;
				if (ec::pb::parse(prec, size, tv)) {
					tv._recpos = pos;
					_plog->add(CLOG_DEFAULT_ALL, "recpos=%u, numidx=%u, rootindxpgno=%jd, tagname=%s",
						tv._recpos, tv._numidx, tv._rootindxpgno, tv._name.c_str());
					_map.set(tv._name.c_str(), std::move(tv));
				}
			});
			_plog->add(CLOG_DEFAULT_ALL, "load records %d from %s", n, sfile);
			return 0;
		}

		/**
		 * @brief 打开表空间，不存在则创建
		 * @param path 表空间路径,可以不存在.
		 * @param name 表空间名
		 * @param pagekiolsize 表控件页面千字节，单位 1024,填写16 表示页面大侠 16 * 1024
		 * @return 0：success； -1:failed
		*/
		int OpenTbs(const char* path, const char* name, int pagekiolsize)//打开表空间，不存在则创建
		{
			if (_tbs.isExist(path, name)) { //存在则打开
				if (0 == _tbs.Open(path, name)) {
					_plog->add(CLOG_DEFAULT_ALL, "Open tablespace path=%s,name=%s success.", path, name);
					return 0;
				}
				_plog->add(CLOG_DEFAULT_ERR, "Open tablespace path=%s,name=%s failed.", path, name);
				return -1;
			}
			//创建
			if (0 == _tbs.Create(path, name, pagekiolsize, DB_INDEX_TBS_FILEKIOLPAGES, INT32_MAX - 1)) {
				_plog->add(CLOG_DEFAULT_ALL, "Create tablespace path=%s,name=%s success.", path, name);
				return 0;
			}
			_plog->add(CLOG_DEFAULT_ERR, "Create tablespace path=%s,name=%s failed.", path, name);
			return -1;
		}

		/**
		 * @brief 写入一个索引入口到rdf文件
		 * @param pidx 索引入口
		 * @return 0: ok; -1:error
		*/
		int writeidx2obf(CTableIndexItem* pidx)
		{
			EC_STACKSTRING(so, DB_IDXOBF_PAGESIZE * 4);//编码输出空间
			if (pidx->_recpos >= 0) { //已分配存储位置
				pidx->out_content(&so);
				if (_obf.writeObject(so.data(), static_cast<int>(so.size()), pidx->_recpos) < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "update tag %s idx to pos %d failed",
						pidx->_name.c_str(), pidx->_recpos);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_ALL, "update tag %s idx to pos %d success",
					pidx->_name.c_str(), pidx->_recpos);
				return 0;
			}
			pidx->_recpos = ec::OBF_PAGE_END;			
			pidx->out_content(&so);
			if (_obf.writeObject( so.data(), static_cast<int>(so.size()), pidx->_recpos) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "write idxobj %s to pos %d failed",
					pidx->_name.c_str(), pidx->_recpos);
				_obf.freeObject(pidx->_recpos);
				pidx->_recpos = -1;
				return -1;
			}
			_plog->add(CLOG_DEFAULT_ALL, "write idxobj %s to pos %d success",
				pidx->_name.c_str(), pidx->_recpos);
			return 0;
		}
	};// class CDataIndex
}// namespace ec
