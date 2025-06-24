/**
* file rdb_table.h
* 实时库历史数据表的读写
* 
\update 
  2025.6.18  写索引增加日志对象参数
  2023-10-26 增加调试接口 CDataTable::foreachDataPage()
  2023-9-28 增加快速插入 CDataTable::insertfast()
\author jiangyong
*/
#pragma once
#include <algorithm>
#include "ec_jsonx.h"
#include "ec_vector.hpp"
#include "ec_dbpagecache.h"
#include "ec_dbindex.h"
#include "ec_dbdatapage.h"

namespace ec {
	constexpr uint16_t RDB_DATAPAGE_MAGIC = 0xCB07; //数据页面魔数
	constexpr uint32_t RDB_REUSE_MIN_IDXNUM = 5;// 重用时的最小页面数,大于3,默认5
	/*!
	\brief 数据表模板类, 用于单个表的读写,_OBJ = pgo_tagval
	*/
	template<class _OBJ>
	class CDataTable
	{
	protected:
		CDataIndex* _pidx; //索引
		ec::tablespace* _pdatatbs;//数据表空间
		ec::ilog* _plog;
		CPageCache _cache; //数据页面缓存
		ec::bytes _pgtmp;
		enum PAGE_WHO{
			PAGE_PRE =0,
			PAGE_NEXT = 1
		};
	public:
		CDataTable(CDataIndex* pidx, ec::tablespace* pdatatbs, ec::ilog* plog) :
			_pidx(pidx),
			_pdatatbs(pdatatbs),
			_plog(plog),
			_cache(pdatatbs) {
			_pgtmp.reserve(16 * 1024);
		}

		virtual ~CDataTable() {
			_cache.FlushAll();
		}

		/*!
		 \brief 插入(或更新)单个对象
		 \param tagname 标签名
		 \param tagid 存储于页面头部的唯一ID
		 \param tagv 对象
		 \return 0:ok; -1: failed
		*/
		int insert(const char* tagname, uint32_t tagid, _OBJ& tagv)
		{
			//先从索引中找
			int64_t ltime = -1, pgno = -1;
			if (_pidx->GetIdx(tagname, tagv.get_idxval(), &ltime, &pgno) < 0) {
				return InertNewTagData(tagname, tagid, tagv); //不存在,插入一个新的标签
			}
			//存在
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed.", pgno);
				return -1;
			}

			CDbDataPage<_OBJ> pgv;
			if (pgv._head.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd), page head parse error at insert(%s,...)", pgno, tagname);
				return -1;
			}
			//解析数据
			pgv.FromPage(page + RDB_DATAPAGE_HEAD_SIZE, pgv._head._size);

			int nr = pgv.Insert(tagv);
			if (-1 == nr) {
				_plog->add(CLOG_DEFAULT_ERR, "tag %s Insert error at insert, idx=%jd,frontidx=%jd,backidx=%jd",
					tagname, tagv.get_idxval(), pgv._objs.front().get_idxval(), pgv._objs.back().get_idxval());
				return -1;
			}

			if (pgv.pgopt_none == nr) //无变化
				return 0;

			size_t sizeres = RDB_DATAPAGE_INSERT_RES_SIZE + RDB_DATAPAGE_HEAD_SIZE;//默认插入保留空间
			if (pgv.pgopt_update == nr) //update
				sizeres = RDB_DATAPAGE_HEAD_SIZE; //不保留空间

			if (pgv.SizeEncode() + sizeres < _pdatatbs->SizePage()) {//没有超出页面
				if (0 != WritePage2Cache(pgno, pgv)) {
					_plog->add(CLOG_DEFAULT_ERR, "update pgno(%jd) at insert(%s,...)", pgno, tagname);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_ALL, "insert %s success,tag(id=%u,name=%s) data pgno=%jd",
					nr == pgv.pgopt_update ? "update" : "insert", tagid, tagname, pgno);
				return 0;
			}

			//分离并插入页面
			int64_t idxvalnew = -1;
			int64_t instpgno = splitsave(tagname, pgno, pgv, tagv.get_idxval() == pgv._objs.back().get_idxval(), idxvalnew);
			if (instpgno < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "insert tag=%s,tagid=%u failed.", tagname, tagid);
				return -1;
			}

			//然后写索引
			if (_pidx->InsertIdx(tagname, idxvalnew, instpgno, tagid, _plog) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) insertTagIdx failed at insert", tagid, tagname);
				return -1;
			}

			_plog->add(CLOG_DEFAULT_ALL, "insert %s and split page success,tag(id=%u,name=%s) data pgno=%jd",
				nr == pgv.pgopt_update ? "update" : "insert", tagid, tagname, pgno);
			return 0;
		}

		/*!
		\brief 优化模式快速插入对象数组
		\param tagname 标签名
		\param tagid 存储于页面头部的唯一ID
		\param objs 对象数组，索引值有序递增, 且没有重复的。
		\param nsize 对象个数
		\param reusepgnum 重用页面数，大于这个数开始重用页面，0表示不使用重用页面功能。
		\return -1: 错误； >=0 追加的对象数(<=nsize); 调用者需要循环调用直到插入完成。
		\remark 原理:找到第一条记录的数据页面page1和下一个数据页面page2，获取page2的开始索引值idx2，将objs数组内小于idx2索引的记录插入page1
		*/
		int insertfast(const char* tagname, uint32_t tagid, const _OBJ* objs, int nsize, uint32_t reusepgnum = 0)
		{
			int i, nap; //可插入记录数
			size_t zlencode, zmaxlen;
			//先从索引中找数据页面
			int64_t ltime = -1, pgno = -1;//数据页面索引和数据页面号。
			if (_pidx->GetIdx(tagname, objs->_time, &ltime, &pgno) < 0) { //不存在，作为新的标签append模式追加记录
				nap = nsize < RDB_DATAPAGE_MAX_NUMOBJS ? nsize : RDB_DATAPAGE_MAX_NUMOBJS; //可插入记录数
				zlencode = 0;
				zmaxlen = _pdatatbs->SizePage() - RDB_DATAPAGE_HEAD_SIZE - RDB_DATAPAGE_INSERT_RES_SIZE; //页面数据最大长度
				for (i = 0; i < nsize && i < RDB_DATAPAGE_MAX_NUMOBJS; i++) { //计算记录长度
					zlencode += objs[i].size_z(_OBJ::get_field_number(), i > 0 ? &objs[i - 1] : nullptr);
					if (zlencode >= zmaxlen) {
						nap = i > 0 ? i : 1;
						break;
					}
				}
				return 0 == AppendNewTagDatas(tagname, tagid, objs, nap) ? nap : -1; //不存在,插入一个新的标签
			}

			//存在则读取数据页面
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed.", pgno);
				return -1;
			}

			CDbDataPage<_OBJ> pgv;//页面对象
			if (pgv._head.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) { //解析页面头
				_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd), page head parse error at append(%s,...)", pgno, tagname);
				return -1;
			}
			int64_t idxnextpage = -1; //下一页的起始索引，-1表示最后一页。
			if (pgv._head._nextpgno >= 0) { //读取下一个页面并解析页面头部的页面索引.
				uint8_t* pagenext = _cache.GetPage(pgv._head._nextpgno);
				if (nullptr == pagenext) {
					_plog->add(CLOG_DEFAULT_ERR, "read nextpage(%jd) failed @ insertfast.", pgv._head._nextpgno);
					return -1;
				}
				CDbPageHead nexthead;
				if (nexthead.frombuf(pagenext, RDB_DATAPAGE_MAGIC) < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd), next page head parse error at append(%s,...) @ insertfast",
						pgv._head._nextpgno, tagname);
					return -1;
				}
				idxnextpage = nexthead._idxval;
			}

			nap = nsize < RDB_DATAPAGE_MAX_NUMOBJS ? nsize : RDB_DATAPAGE_MAX_NUMOBJS; //可插入记录数
			zlencode = pgv._head._size + RDB_DATAPAGE_HEAD_SIZE + RDB_DATAPAGE_INSERT_RES_SIZE;//初始化为原页面长度.
			zmaxlen = _pdatatbs->SizePage() + _pdatatbs->SizePage() / 2u; // 插入后总长度1.5倍页面，可安全分页。
			for (i = 0; i < nsize && i < RDB_DATAPAGE_MAX_NUMOBJS; i++) {
				if (idxnextpage >= 0 && objs[i].get_idxval() >= idxnextpage) { //索引超下一个页面索引，确定nap插入记录数
					nap = i > 0 ? i : 1;
					break;
				}
				zlencode += objs[i].size_z(_OBJ::get_field_number(), i > 0 ? &objs[i - 1] : nullptr);
				if (zlencode >= zmaxlen) { //超过编码后长度限制,确定nap记录数
					nap = i > 0 ? i : 1;
					break;
				}
			}

			//解析并插入数据
			pgv.FromPage(page + RDB_DATAPAGE_HEAD_SIZE, pgv._head._size);//解析原页面记录
			if (pgv._objs.empty() || objs->get_idxval() > pgv._objs.back().get_idxval()) { //追加模式优化
				pgv._objs.insert(pgv._objs.end(), objs, objs + nap);
			}
			else { //插入模式
				ec::vector<int> vflag;//替换标记，0没有替换,1替换了。
				vflag.resize(nap);
				int nrep = 0;
				for (i = 0; i < nap; i++) {
					vflag[i] = vreplace(pgv._objs.data(), (int)pgv._objs.size(), objs[i]);
					if (vflag[i])
						nrep++;
				}

				if (!nrep) { //没有产生替换,尾部插入
					pgv._objs.insert(pgv._objs.end(), objs, objs + nap);
				}
				else { //产生替换，追加未替换的
					for (i = 0; i < nap; i++) {
						if (!vflag[i]) {
							pgv._objs.push_back(objs[i]);
						}
					}
				}
				std::sort(pgv._objs.begin(), pgv._objs.end(), [](const _OBJ& v1, const _OBJ& v2) {
					return v1.get_idxval() < v2.get_idxval();
				});//重新排序
			}

			if (pgv.SizeEncode() + RDB_DATAPAGE_INSERT_RES_SIZE + RDB_DATAPAGE_HEAD_SIZE < _pdatatbs->SizePage()) {
				if (0 != WritePage2Cache(pgno, pgv)) {
					_plog->add(CLOG_DEFAULT_ERR, "update pgno(%jd) at insert(%s,...)", pgno, tagname);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_ALL, "insert success,tag(id=%u,name=%s) data pgno=%jd, number objs=%d",
					tagid, tagname, pgno, nap);
				return nap;
			}

			//分离并插入页面
			int64_t idxvalnew = -1;
			int64_t instpgno = splitsave(tagname, pgno, pgv, true, idxvalnew, reusepgnum);
			if (instpgno < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "insert tag=%s,tagid=%u failed.", tagname, tagid);
				return -1;
			}

			//然后写索引
			if (_pidx->InsertIdx(tagname, idxvalnew, instpgno, tagid, _plog) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) InsertTagIdx failed at insertfast", tagid, tagname);
				return -1;
			}

			_plog->add(CLOG_DEFAULT_ALL, "insert and split page success,tag(id=%u,name=%s) data pgno=%jd,append objs %d; insertidx(%jd,%jd)",
				tagid, tagname, pgno, nap, idxvalnew, instpgno);
			return nap;
		}

		/*!
		\brief 优化模式快速追加对象数组, 已废弃，统一使用insertfast
		\param tagname 标签名
		\param tagid 存储于页面头部的唯一ID
		\param objs 对象数组
		\param nsize 对象个数
		\return >=0 追加的对象数; -1:错误
		\remark objs里的idx一定是递增的,一定是大于目前库中最大idx的。
		*/
		int append_discarded(const char* tagname, uint32_t tagid, const _OBJ* objs, int nsize, uint32_t reusepgno = 0)
		{
			int i, nap; //可插入记录数,按照半个页面大小每次
			size_t zlencode, zmaxlen;
			
			//先从索引中找
			int64_t ltime = -1, pgno = -1;
			if (_pidx->GetIdx(tagname, objs->_time, &ltime, &pgno) < 0) {
				nap = nsize; //可插入记录数,按照半个页面大小每次
				zlencode = 0;
				zmaxlen = _pdatatbs->SizePage() - RDB_DATAPAGE_HEAD_SIZE - RDB_DATAPAGE_INSERT_RES_SIZE;
				for (i = 0; i < nsize; i++) {
					zlencode += objs[i].size_z(_OBJ::get_field_number(), i > 0 ? &objs[i - 1] : nullptr);
					if (zlencode >= zmaxlen) {
						nap = i > 0 ? i : 1;
						break;
					}
				}
				return 0 == AppendNewTagDatas(tagname, tagid, objs, nap) ? nap : -1; //不存在,插入一个新的标签
			}

			//存在
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed.", pgno);
				return -1;
			}

			CDbDataPage<_OBJ> pgv;
			if (pgv._head.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd), page head parse error at append(%s,...)", pgno, tagname);
				return -1;
			}

			nap = nsize; //可插入记录数,默认可全部插入
			zlencode = pgv._head._size + RDB_DATAPAGE_HEAD_SIZE + RDB_DATAPAGE_INSERT_RES_SIZE;
			zmaxlen = _pdatatbs->SizePage() + _pdatatbs->SizePage() / 2u; // 1.5倍
			for (i = 0; i < nsize; i++) {
				zlencode += objs[i].size_z(_OBJ::get_field_number(), i > 0 ? &objs[i - 1] : nullptr);
				if (zlencode >= zmaxlen) {
					nap = i > 0 ? i : 1;
					break;
				}
			}

			//解析数据
			pgv.FromPage(page + RDB_DATAPAGE_HEAD_SIZE, pgv._head._size);
			pgv._objs.insert(pgv._objs.end(), objs, objs + nap);

			if (pgv.SizeEncode() + RDB_DATAPAGE_INSERT_RES_SIZE + RDB_DATAPAGE_HEAD_SIZE < _pdatatbs->SizePage()) {
				if (0 != WritePage2Cache(pgno, pgv)) {
					_plog->add(CLOG_DEFAULT_ERR, "update pgno(%jd) at append(%s,...)", pgno, tagname);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_ALL, "append success,tag(id=%u,name=%s) data pgno=%jd, number objs=%d",
					tagid, tagname, pgno, nap);
				return nap;
			}

			//分离并插入页面
			int64_t idxvalnew = -1;
			int64_t instpgno = splitsave(tagname, pgno, pgv, true, idxvalnew, reusepgno);
			if (instpgno < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "append tag=%s,tagid=%u failed.", tagname, tagid);
				return -1;
			}

			//然后写索引
			if (_pidx->InsertIdx(tagname, idxvalnew, instpgno, tagid, _plog) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) InsertTagIdx failed at append", tagid, tagname);
				return -1;
			}

			_plog->add(CLOG_DEFAULT_ALL, "append and split page success,tag(id=%u,name=%s) data pgno=%jd,append objs %d; insertidx(%jd,%jd)",
				tagid, tagname, pgno, nap, idxvalnew, instpgno);
			return nap;
		}

		/*!
		\brief 查询对象历史
		\param tagname 标签名
		\param idxv 起始索引值
		\param fun 遍历回调, 返回0继续遍历, 非0表示终止遍历
		\param pdataEnd 如果查询全部数据，输出置1；否则置0
		\param includepreone 0：不包含前一个记录；非0：包含idxv的前一个记录(用于计算插值)
		*/
		int query(const char* tagname, int64_t idxv, std::function<int(_OBJ& tagv)>fun, int* pdataEnd = nullptr, bool includepreone = false)
		{
			int64_t ltime = -1, pgno = -1, idxs = idxv;
			if (includepreone && idxs > 0) {
				--idxs;
			}
			if (_pidx->GetIdx(tagname, idxs, &ltime, &pgno) < 0)
				return 0;
			int nfunret = 0, numrecs = 0;
			while (pgno >= 0 && !nfunret) {
				uint8_t* page = _cache.GetPage(pgno);
				if (nullptr == page) {
					_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed @query tag=%s", pgno, tagname);
					return -1;
				}
				CDbDataPage<_OBJ> pgv;
				if (pgv._head.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "parse pgno(%jd) page head error @query tag=%s", pgno, tagname);
					return -1;
				}
				if (pgv.FromPage(page + RDB_DATAPAGE_HEAD_SIZE, pgv._head._size) < 0)//解析数据
					_plog->add(CLOG_DEFAULT_ERR, "parse pgno(%jd) data record failed", pgno);
				else {
					//_plog->add(CLOG_DEFAULT_ALL, "query pageno(%jd) numrecords %zu", pgno, pgv._objs.size());
					for (auto i = 0u; i < pgv._objs.size(); i++) {
						if (pgv._objs[i].get_idxval() >= idxv) {
							if (includepreone && i > 0 && !numrecs) {
								if (0 != (nfunret = fun(pgv._objs[i - 1])))
									break;
								++numrecs;
							}
							if (0 != (nfunret = fun(pgv._objs[i])))
								break;
							++numrecs;
						}
					}
				}
				pgno = pgv._head._nextpgno;
			}
			if (pdataEnd && pgno < 0)
				*pdataEnd = 1;
			return 0;
		}

		/**
		 * @brief 删除指定索引的记录
		 * @param tagname 标签名
		 * @param idxv 索引值
		 * @return 0:success; -1:error; 1:no record
		*/
		int deleterecord(const char* tagname, int64_t idxv)
		{
			int64_t ltime = -1, pgno = -1; //数据页面
			if (_pidx->GetIdx(tagname, idxv, &ltime, &pgno) < 0)
				return 1; //获取数据页面号失败
			if (pgno < 0)
				return 1;
			//读取页面数据
			CDbDataPage<_OBJ> pgv;
			if (GetPageDatas(pgno, pgv) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "GetPageDatas page(%jd) failed @deleterecord tag=%s", pgno, tagname);
				return -1;
			}
			_plog->add(CLOG_DEFAULT_ALL, "get pageno(%jd) numrecords %zu", pgno, pgv._objs.size());
			if (pgv._objs.empty()) {
				if(pgv._head._prevpgno >= 0 || pgv._head._nextpgno >= 0)
					_plog->add(CLOG_DEFAULT_ERR, "GetPageDatas page(%jd) empty page not first page @deleterecord tag=%s", pgno, tagname);
				return 1; //no record
			}
			auto idel = FindFast(pgv._objs, idxv);
			if (idel == pgv._objs.end())
				return 1; // no record
			pgv._objs.erase(idel);

			if (!pgv._objs.empty()) { //非空,直接写回数据页面
				if (0 != WritePage2Cache(pgno, pgv)) {
					_plog->add(CLOG_DEFAULT_ERR, "WritePage2Cache page(%jd) failed @deleterecord not empty tag=%s", pgno, tagname);
					return -1;
				}
				return 0;
			}
			//下面需要删除页面和索引，先刷缓存
			int nferr = _cache.FlushAll();
			if (nferr > 0)
				_plog->add(CLOG_DEFAULT_ERR, "Begin FlushAll error deleterecord tag(%s). pgno(%jd))", tagname, pgno);

			if (pgv._head._prevpgno >= 0) { //删除的不是头页面
				if (_pidx->DelIdxRec(tagname, pgv._head._idxval, pgno) < 0) { //先删除索引
					_plog->add(CLOG_DEFAULT_ERR, "Delete idx(idx=%jd,pgno=%jd) error @deleterecord tag(%s). pgno(%jd))",
						pgv._head._idxval, pgno, tagname, pgno);
					return -1;
				}
				//再修改数据页面连接
				if (modifydatapageptr(pgv._head._prevpgno, PAGE_NEXT, pgv._head._nextpgno) ||
					(pgv._head._nextpgno >= 0 && modifydatapageptr(pgv._head._nextpgno, PAGE_PRE, pgv._head._prevpgno))
						) {
					_cache.clear();//失败，清空缓存，不落地
					return -1;
				}
				//最后删除数据页面
				_cache.RemovePage(pgno);
				_pdatatbs->pagefree(pgno);
				_cache.FlushAll();
				return 0;
			}

			//下面是头页面特殊处理
			if (pgv._head._nextpgno < 0) { //没有下一页，说明全部空，删除数据页面和索引
				_pidx->ClearIdxTree(tagname, [](int64_t idxv, int64_t pgno) {});
				_cache.RemovePage(pgno);
				_pdatatbs->pagefree(pgno);
				_cache.FlushAll();
				return 0;
			}

			CDbDataPage<_OBJ> pgv2;//复制第二页面的数据。删除第二页面和索引，因为首页的索引始终为0(最小)，首页索引不动
			int64_t pgno2 = pgv._head._nextpgno;
			if (GetPageDatas(pgno2, pgv2) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "GetPageDatas pgno2 page(%jd) failed @deleterecord tag=%s", pgno2, tagname);
				return -1;
			}

			pgv._objs.swap(pgv2._objs);//交换数据
			pgv._head._nextpgno = pgv2._head._nextpgno;
			if (0 != WritePage2Cache(pgno, pgv)) {
				_plog->add(CLOG_DEFAULT_ERR, "WritePage2Cache page(%jd) failed @deleterecord tag=%s", pgno, tagname);
				return -1;
			}
			if (pgv2._head._nextpgno >= 0)
				modifydatapageptr(pgv2._head._nextpgno, PAGE_PRE, pgno);

			_cache.RemovePage(pgno2);//删除页面
			_pdatatbs->pagefree(pgno2);
			_cache.FlushAll();

			_pidx->DelIdxRec(tagname, pgv2._head._idxval, pgno2); //删除索引
			return 0;
		}

		/**
		 * @brief 遍历标签的所有索引信息和页面信息，用于调试排错
		 * @param stagname 标签名
		 * @param sout 输出的字符串, 输出一个JSON对象数组[]
		 * @param idxtime 是否将索引当作时标输出，0：否； 1：是
		 * @return 0：success； -1：无此标签
		*/
		template<class String = std::string>
		int foreachDataPage(const char* stagname, String& sout, int idxtime = 0)
		{
			int n = 0;
			sout.push_back('[');
			int nret = _pidx->ForEachDataIdx(stagname, [&](int64_t idxv, int64_t pgno) {
				if (n) {
					sout.push_back(',');
				}
				sout.push_back('{');
				int nf = 0;
				
				//读取数据页面
				CDbDataPage<_OBJ> pgv;
				if (GetPageDatas(pgno, pgv) < 0) {
					ec::js::out_jnumber(nf, "idx.idxval", idxv, sout, true);
					if (idxtime) {
						ec::js::out_jtime(nf, "idx.idxtime", idxv, sout, ECTIME_ISOSTR);
					}
					ec::js::out_jnumber(nf, "idx.pgno", pgno, sout, true);
					ec::js::out_jstring(nf, "status", "GetPageDatas failed", sout);
				}
				else {
					ec::js::out_jnumber(nf, "idx.idxval", idxv, sout, true);
					if (idxtime) {
						ec::js::out_jtime(nf, "idx.idxtime", idxv, sout, ECTIME_ISOSTR);
					}
					ec::js::out_jnumber(nf, "head.idxval", pgv._head._idxval, sout, true);
					if (idxtime) {
						ec::js::out_jtime(nf, "head.idxtime", pgv._head._idxval, sout, ECTIME_ISOSTR);
					}
					ec::js::out_jnumber(nf, "idx.pgno", pgno, sout, true);
					ec::js::out_jnumber(nf, "head.prevpgno", pgv._head._prevpgno, sout, true);
					ec::js::out_jnumber(nf, "head.nextpgno", pgv._head._nextpgno, sout, true);

					ec::js::out_jnumber(nf, "head.size", pgv._head._size, sout, true);
					ec::js::out_jnumber(nf, "head.numrecs", pgv._head._numrecs, sout, true);
					ec::js::out_jnumber(nf, "head.objid", pgv._head._objid, sout, true);
					ec::js::out_jnumber(nf, "objs.size", pgv._objs.size(), sout, true);
					if (pgv._objs.size() > 0) {
						ec::js::out_jnumber(nf, "objs0.idxval", pgv._objs.front().get_idxval(), sout, true);
						if (idxtime) {
							ec::js::out_jtime(nf, "objs0.idxtime", pgv._objs.front().get_idxval(), sout, ECTIME_ISOSTR);
						}
					}
				}
				sout.push_back('}');
				++n;
				});
			sout.push_back(']');
			return nret;
		}

		/**
		 * @brief 删除标签历史数据
		 * @param stagname 标签名
		 * @return 释放的页面数。
		*/
		int DeleteTag(const char* stagname)
		{
			int n = 0;
			_pidx->ClearIdxTree(stagname, [&](int64_t idxv, int64_t pgno) {
				++n;
				_pdatatbs->pagefree(pgno);
				});
			return n;
		}
	protected:
		/**
		 * @brief 写一个新标签的数据记录
		 * @param tagname 标签名
		 * @param tagid 内部id
		 * @param tagv 记录
		 * @return 0：success；-1：error
		*/
		int InertNewTagData(const char* tagname, uint32_t tagid, _OBJ& tagv)
		{
			int64_t pgno = _pdatatbs->pagealloc();
			if (pgno < 0)
				return -1;
			CDbDataPage<_OBJ> pgv;
			if (pgv.Insert(tagv) < 0)
				return -1;
			pgv._head._objid = tagid;
			pgv._head._idxval = 0;
			pgv._head._nextpgno = -1;
			pgv._head._prevpgno = -1;
			pgv._head._flag = RDB_DATAPAGE_MAGIC;
			pgv._head._numrecs = (uint16_t)pgv._objs.size();

			//写数据页面
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_pdatatbs->pagefree(pgno);
				return -1;
			}
			if (0 != WritePage2Cache(pgno, pgv) || 0 != _cache.Flush(pgno)) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) inertnewtagval failed", tagid, tagname);
				_pdatatbs->pagefree(pgno);
				return -1;
			}
			//然后写索引
			if (_pidx->InsertIdx(tagname, 0, pgno, tagid, _plog) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) insertTagIdx failed at inertnewtagval", tagid, tagname);
				_pdatatbs->pagefree(pgno);
				return -1;
			}
			_plog->add(CLOG_DEFAULT_ALL, "inertnewtagval success,tag(id=%u,name=%s) data pgno=%jd", tagid, tagname, pgno);
			return 0;
		}

		/**
		 * @brief 写新标签记录集
		 * @param tagname 标签名
		 * @param tagid 内部id
		 * @param ptagv 记录集，保证所有记录编码后能存入一个页面
		 * @param nsize 记录数(小于65535)
		 * @return 0：success；-1：error
		*/
		int AppendNewTagDatas(const char* tagname, uint32_t tagid, const _OBJ* ptagv, int nsize)
		{
			int64_t pgno = _pdatatbs->pagealloc();
			if (pgno < 0)
				return -1;
			CDbDataPage<_OBJ> pgv;
			pgv._objs.assign(ptagv, ptagv + nsize);

			pgv._head._objid = tagid;
			pgv._head._idxval = 0;
			pgv._head._nextpgno = -1;
			pgv._head._prevpgno = -1;
			pgv._head._flag = RDB_DATAPAGE_MAGIC;
			pgv._head._numrecs = (uint16_t)pgv._objs.size();

			//写数据页面
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_pdatatbs->pagefree(pgno);
				return -1;
			}
			if (0 != WritePage2Cache(pgno, pgv) || 0 != _cache.Flush(pgno)) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) appendnewtagvals failed", tagid, tagname);
				_pdatatbs->pagefree(pgno);
				return -1;
			}
			//然后写索引
			if (_pidx->InsertIdx(tagname, 0, pgno, tagid, _plog) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "tag(id=%u,name=%s) insertTagIdx failed @appendnewtagvals", tagid, tagname);
				_pdatatbs->pagefree(pgno);
				return -1;
			}
			_plog->add(CLOG_DEFAULT_ALL, "appendnewtagvals success,tag(id=%u,name=%s) data pgno=%jd", tagid, tagname, pgno);
			return 0;
		}
	private:
		/**
		 * @brief 修改数据页面先后页连接
		 * @param pgno 欲操作的数据页面号
		 * @param pgwho  改变页面连接， PAGE_NEXT或者PAGE_PRE ，
		 * @param pgno2  修改后的连接页面号
		 * @return 0:suzccess; -1:error
		*/
		int modifydatapageptr(int64_t pgno, PAGE_WHO pgwho, int64_t pgno2)
		{
			CDbDataPage<_OBJ> pgv;
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed.", pgno);
				return -1;
			}
			if (pgv._head.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "parse pgno(%jd) head error", pgno);
				return -1;
			}
			if (pgwho == PAGE_NEXT)
				pgv._head._nextpgno = pgno2;
			else
				pgv._head._prevpgno = pgno2;

			return WriteHead2Cache(pgno, pgv._head);
		}

		/**
		 * @brief 读页面并解析到到页面对象
		 * @param pgno 数据页面号
		 * @param pgv 数据页面对象
		 * @return 0:suzccess; -1:error
		*/
		int GetPageDatas(int64_t pgno, CDbDataPage<_OBJ>& pgv)
		{
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed.", pgno);
				return -1;
			}
			if (pgv._head.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "parse pgno(%jd) head error", pgno);
				return -1;
			}
			if (pgv.FromPage(page + RDB_DATAPAGE_HEAD_SIZE, pgv._head._size) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "parse pgno(%jd) data record error", pgno);
				return -1;
			}
			return 0;
		}

		/**
		 * @brief 读取数据页面仅解析头部
		 * @param pgno 数据页面号
		 * @param pgh 头部对象
		 * @return 0:suzccess; -1:error
		*/
		int GetPageHead(int64_t pgno, CDbPageHead& pgh)
		{
			uint8_t* page = _cache.GetPage(pgno);
			if (nullptr == page) {
				_plog->add(CLOG_DEFAULT_ERR, "read page(%jd) failed.", pgno);
				return -1;
			}
			if (pgh.frombuf(page, RDB_DATAPAGE_MAGIC) < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "parse pgno(%jd) head error", pgno);
				return -1;
			}
			return 0;
		}

		/**
		 * @brief 编码并写页面对象到缓冲
		 * @param pgno 数据页面号
		 * @param pgv 页面对象
		 * @return 0:suzccess; -1:error
		*/
		int WritePage2Cache(int64_t pgno, CDbDataPage<_OBJ>& pgv)
		{
			_pgtmp.clear();
			uint8_t pgh[RDB_DATAPAGE_HEAD_SIZE] = { 0 };
			_pgtmp.append(pgh, RDB_DATAPAGE_HEAD_SIZE);//添加站位
			pgv.OutPage(&_pgtmp);//先写数据填写size和numrec.
			pgv._head.tobuf(_pgtmp.data(), RDB_DATAPAGE_MAGIC);//再写头部
			return _cache.WritePage(pgno, 0, _pgtmp.data(), _pgtmp.size());
		}

		/**
		 * @brief 编码并写页面对象头部(40字节)
		 * @param pgno 数据页面号
		 * @param pgh 头部对象
		 * @return 0:suzccess; -1:error
		*/
		int WriteHead2Cache(int64_t pgno, CDbPageHead& pgh)
		{
			uint8_t pghbuf[RDB_DATAPAGE_HEAD_SIZE] = { 0 };
			pgh.tobuf(pghbuf, RDB_DATAPAGE_MAGIC);//再写头部
			return _cache.WritePage(pgno, 0, pghbuf, RDB_DATAPAGE_HEAD_SIZE);
		}

		/**
		 * @brief 分页并编码后写入数据页面，分页前和后都将刷盘
		 * @param tagname 标签名
		 * @param pgno 数据页面号
		 * @param pgv 页面对象
		 * @param binc 是否是递增分页，是则3/4处分页，否则1/4处分页
		 * @param newpageidxval 返回的新页的索引值
		 * @param reusepgnum 重用最低页面数，默认0表示不使用重用。
		 * @return 返回分配的第二个页面的页面号,调用者用于写索引。 返回-1表示失败。
		*/
		int64_t splitsave(const char* tagname, int64_t pgno, CDbDataPage<_OBJ>& pgv, bool binc, int64_t& newpageidxval, uint32_t reusepgnum = 0)
		{
			//先分页
			CDbDataPage<_OBJ> pg2rd;
			if (!pgv.SplitPage(pg2rd._objs, _pdatatbs->SizePage(), binc) || pg2rd._objs.empty()) {
				_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd), page split return 0 splitsave(%s,...)", pgno, tagname);
				return -1;
			}

			//将新页分配一个页面,肯定不在缓冲里,先写入磁盘
			int64_t pg2rdno = -1;
			if (reusepgnum >= RDB_REUSE_MIN_IDXNUM && _pidx->GetIdxNum(tagname) >= reusepgnum)
				pg2rdno = reuse(tagname, pgno);

			if (pg2rdno == -1)
				pg2rdno = _pdatatbs->pagealloc();

			if (pg2rdno < 0) {
				_plog->add(CLOG_DEFAULT_ERR, "pagealloc error splitsave tag(%s). pgno(%jd))", tagname, pgno);
				return -1;
			}

			//开始分页操作,先将缓存刷新到磁盘;
			if (0 != _cache.FlushAll()) {
				_plog->add(CLOG_DEFAULT_ERR, "Begin FlushAll error splitsave tag(%s). pgno(%jd))", tagname, pgno);
				_pdatatbs->pagefree(pg2rdno);
				return -1;
			}

			pg2rd._head._idxval = pg2rd._objs.front().get_idxval();//新页面的索引值是固定不变的。
			pg2rd._head._nextpgno = pgv._head._nextpgno;
			pg2rd._head._prevpgno = pgno;

			if (0 != WritePage2Cache(pg2rdno, pg2rd)) {
				_plog->add(CLOG_DEFAULT_ERR, "Wpg2cache pg2rdno(%jd), error splitsave(%s,...)", pg2rdno, tagname);
				_pdatatbs->pagefree(pg2rdno);
				_cache.RemovePage(pg2rdno);
				return -1;
			}

			if (pg2rd._head._nextpgno != -1) { //更新后一个页面的前连接
				CDbPageHead pghnext;
				if (0 != GetPageHead(pg2rd._head._nextpgno, pghnext)) {
					_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd) getpagehead failed to update head", pg2rd._head._nextpgno);
					_cache.RemovePage(pg2rdno);
					_pdatatbs->pagefree(pg2rdno);
					return -1;
				}
				pghnext._prevpgno = pg2rdno;
				if (0 != WriteHead2Cache(pg2rd._head._nextpgno, pghnext)) {
					_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd) updatehead2cache failed to update head", pg2rd._head._nextpgno);
					_cache.RemovePage(pg2rd._head._nextpgno);
					_cache.RemovePage(pg2rdno);
					_pdatatbs->pagefree(pg2rdno);
					return -1;
				}
			}

			//更新pgno的后连接,并将页面头和数据写入缓冲。
			pgv._head._nextpgno = pg2rdno;
			if (0 != WritePage2Cache(pgno, pgv)) {
				_plog->add(CLOG_DEFAULT_ERR, "Wpg2cache pg2rdno(%jd), error splitsave(%s,...)", pg2rdno, tagname);
				_cache.RemovePage(pg2rd._head._nextpgno);
				_cache.RemovePage(pg2rdno);
				_pdatatbs->pagefree(pg2rdno);
				return -1;
			}

			//刷新所有页面到磁盘
			if (0 != _cache.FlushAll()) {
				_plog->add(CLOG_DEFAULT_ERR, "End FlushAll failed at splitsave tag %s", tagname);
				_cache.RemovePage(pg2rdno);
				_pdatatbs->pagefree(pg2rdno);
				return -1;
			}
			newpageidxval = pg2rd._head._idxval;
			return pg2rdno;
		}

		/*!
		重用最旧的页面
		  为了不改动入口页面和第一个页面indxval=0的规则, 操作方法:
		  1)将第三页面的前连接改为第一页面,只改头部
		  2)将第二个页面的数据复制到第一个页面;更改前连接为-1,写入到第一页面
		  3)返回可用的第二页面。
		  整个操作更改第一个页面和第三个页面.

		\param tagname 标签名
		\param excludepgno 排除的页面号,防止重用自己
		\return 返回-1表示没有可重用的页面, >=0为可重用的页面号,应用层可直接使用。
		\remark 返回的页面直接可用(相当于_pdatatbs->pagealloc),并没有放到可用连接表里,
		  如果不使用，需要使用_pdatatbs->pagefree()释放。
		*/
		int64_t reuse(const char* tagname, int64_t excludepgno)
		{
			int64_t rtpgno = _pidx->GetRootDataPgNo(tagname);
			if (rtpgno < 0)
				return -1;
			CDbDataPage<_OBJ> pgroot, pg2nd;

			if (0 != GetPageDatas(rtpgno, pgroot)) {
				_plog->add(CLOG_DEFAULT_ERR, "Get root data page datas failed @reuse(%s)", tagname);
				return -1;
			}
			if (pgroot._head._nextpgno < 0) {
				_plog->add(CLOG_DEFAULT_MSG, "pgroot._head._nextpgno < 0 @reuse(%s)", tagname);
				return -1;
			}

			if (pgroot._head._nextpgno == excludepgno) {
				_plog->add(CLOG_DEFAULT_MSG, "pgroot._head._nextpgno == excludepgno @reuse(%s)", tagname);
				return -1;
			}

			if (0 != GetPageDatas(pgroot._head._nextpgno, pg2nd)) {
				_plog->add(CLOG_DEFAULT_ERR, "Get 2rd data page datas failed @reuse(%s)", tagname);
				return -1;
			}

			//开始操作前先将缓存刷新到磁盘;
			if (0 != _cache.FlushAll()) {
				_plog->add(CLOG_DEFAULT_ERR, "Begin FlushAll failed @reuse(%s)", tagname);
				return -1;
			}
			_plog->add(CLOG_DEFAULT_ALL, "start 1st_pgno(%jd) 2nd_pgno(%jd) 3rd_pgno(%jd)",
				rtpgno, pgroot._head._nextpgno, pg2nd._head._nextpgno);

			if (pg2nd._head._nextpgno != -1) { //更新第三个页面的前连接为root页面
				CDbPageHead pghnext;
				if (0 != GetPageHead(pg2nd._head._nextpgno, pghnext)) {
					_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd) getpagehead failed to update head @reuse(%s)",
						pg2nd._head._nextpgno, tagname);
					return -1;
				}
				pghnext._prevpgno = rtpgno;
				if (0 != WriteHead2Cache(pg2nd._head._nextpgno, pghnext)) {
					_plog->add(CLOG_DEFAULT_ERR, "pgno(%jd) updatehead2cache failed to update head@reuse(%s)",
						pg2nd._head._nextpgno, tagname);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_ALL, "update 3rd pgno(%jd) prep pgno to (%jd)", pg2nd._head._nextpgno, rtpgno);
			}
			int64_t idxv2nd = pg2nd._head._idxval;//备份供后面删除
			pg2nd._head._prevpgno = -1; //更改连接
			pg2nd._head._idxval = pgroot._head._idxval; //更改索引值
			if (0 != WritePage2Cache(rtpgno, pg2nd)) {// 将第二页面写入root页面
				_cache.RemovePage(pg2nd._head._nextpgno);//恢复前面更改的连接
				return -1;
			}

			_cache.RemovePage(pgroot._head._nextpgno);//刷新所有页面到磁盘, 前需要先从缓存释放2nd页面
			if (0 != _cache.FlushAll()) {
				_plog->add(CLOG_DEFAULT_ERR, "End FlushAll failed failed @reuse(%s)", tagname);
				_cache.RemovePage(rtpgno);
				_cache.RemovePage(pg2nd._head._nextpgno);
				return -1;
			}
			if (0 != _pidx->DelIdxRec(tagname, idxv2nd, pgroot._head._nextpgno))//精确删除索引
				_plog->add(CLOG_DEFAULT_ERR, "delete idx(%jd,%jd) failed", idxv2nd, pgroot._head._nextpgno);
			else
				_plog->add(CLOG_DEFAULT_ALL, "delete idx(%jd,%jd) success", idxv2nd, pgroot._head._nextpgno);
			_plog->add(CLOG_DEFAULT_ALL, "success 1st_pgno(%jd) 2nd_pgno(%jd), return free pgno(%jd)",
				rtpgno, pg2nd._head._nextpgno, pgroot._head._nextpgno);
			return pgroot._head._nextpgno;
		}

		/**
		 * @brief 查找替换
		 * @param objs 记录集
		 * @param nsize 记录数
		 * @param v 插入替换的记录
		 * @return 0：没有替换；1：替换
		*/
		int vreplace(_OBJ* objs, int nsize, const _OBJ& v)
		{
			int64_t idx = v.get_idxval();
			if (nsize < 10) { //小规模直接遍历
				for (auto i = 0; i < nsize; i++) {
					if (objs[i].get_idxval() == idx) {
						objs[i] = v;
						return 1;
					}
				}
			}
			int nl = 0, nh = nsize - 1, nm = (nl + nh) / 2;
			while (nl <= nh) { //二分查找替换
				nm = (nl + nh) / 2;
				if (objs[nm].get_idxval() == idx) {
					objs[nm] = v;
					return 1;
				}
				else if (objs[nm].get_idxval() < idx) {
					nl = nm + 1;
				}
				else {
					nh = nm - 1;
				}
			}
			return 0;
		}

		/**
		 * @brief 二分快速查找
		 * @param objs 集合
		 * @param idx 需要查找的索引
		 * @return 返回迭代器，如果没有找到返回 objs.end()
		*/
		typename ec::vector<_OBJ>::iterator FindFast(ec::vector<_OBJ>& objs, int64_t idx)
		{
			int nsize = (int)objs.size();
			if (nsize < 10) { //小规模直接遍历
				for (auto i = 0; i < nsize; i++) {
					if (objs[i].get_idxval() == idx) {
						return objs.begin() + i;
					}
				}
				return objs.end();
			}
			int nl = 0, nh = nsize - 1, nm = (nl + nh) / 2;
			while (nl <= nh) { //二分查找替换
				nm = (nl + nh) / 2;
				if (objs[nm].get_idxval() == idx) {
					return objs.begin() + nm;
				}
				else if (objs[nm].get_idxval() < idx) {
					nl = nm + 1;
				}
				else {
					nh = nm - 1;
				}
			}
			return objs.end();
		}
	};
}//namespace rdb