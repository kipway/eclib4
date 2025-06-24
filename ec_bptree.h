/*!
\brief ec_bptree.h
B+ tree template class

\author	jiangyong
\email  kipway@outlook.com
\update 
  2025.6.18  页面大小改从表空间获取
  2024.11.11 support no ec_alloctor

eclib 3.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once

#include <assert.h>
#include <stdint.h>
#include <vector>
#include <algorithm>
#include "ec_memory.h"
#include "ec_stream.h"
#include "ec_ipgstorage.h"

#define BPTREE_PAGE_IDX   0x7251 //索引页,item的pgno指向下一个索引页面号
#define BPTREE_PAGE_LEAF  0x7353 //叶子页,叶子页的pgno指向数据页号

#define BPTREE_SUCCESS 0    //成功
#define BPTREE_FAILED  (-1) //失败

#define BPTREE_ITEM_NOTEXIST  0 //不存在
#define BPTREE_ITEM_NOTCHANGE 1 //未改变
#define BPTREE_ITEM_UPDATED   2 //已更新
#define BPTREE_ITEM_INSERTED  3 //已插入

#define BPTREE_LNK_LEFT  0 //左连接
#define BPTREE_LNK_RIGHT 1 //右连接
namespace ec {
	template<typename _Tpidxv>
	class min_idxv_
	{
	public:
		bool ismin(_Tpidxv& v) {
			return 0 == v;
		}
		_Tpidxv minidxv() {
			return 0;
		}
	};

	//B+树索引类
	// >= k1   >= k2   >=k3     >=k4
	// page1   page2   page3    page4
	// _Tpidxv >0 , _Tppgno >0 , 作为最左记录的页面索引始终为最小。

	template<
		typename _Tpidxv = int64_t, //索引值,比如timestamp
		typename _Tppgno = int64_t, //页面号
		class _MixIdxv = min_idxv_<_Tpidxv> //索引值比较
	>class btree
	{
	public:
#ifdef _DEBUG
		void tracetree(const char* format, ...)
		{
			int nbytes = 0;
			char stmp[4096];
			va_list arg_ptr;
			va_start(arg_ptr, format);
			nbytes = vsnprintf(stmp, 4096 - 8, format, arg_ptr);
			va_end(arg_ptr);

			if (nbytes <= 0)
				return;
			stmp[nbytes] = 0;
			fprintf(stderr, "%s", stmp);
		}
#else
#define tracetree(fmt, ...) {}
#endif

#if __GNUG__ && __GNUC__ < 5
		using typeidxv = typename std::enable_if<__has_trivial_copy(_Tpidxv), _Tpidxv>::type;
		using typepgno = typename std::enable_if<__has_trivial_copy(_Tppgno), _Tppgno>::type;
#else
		using typeidxv = typename std::enable_if<std::is_trivially_copyable<_Tpidxv>::value, _Tpidxv>::type;
		using typepgno = typename std::enable_if<std::is_trivially_copyable<_Tppgno>::value, _Tppgno>::type;
#endif
		struct t_item //索引记录
		{
			typeidxv idxv; //索引值
			typepgno pgno; //指向的页面号(非叶子节点指向下一级索引页面，叶子节点指向数据页面)
			t_item() {
				idxv = 0;
				pgno = EC_PGF_ENDNO;
			}
			t_item(const typeidxv& idx, typepgno no = EC_PGF_ENDNO) {
				idxv = idx;
				pgno = no;
			}
		};

		struct t_pghead //页面头部
		{
			uint16_t flag;  //标记，BPTREE_PAGE_IDX或者BPTREE_PAGE_LEAF
			uint16_t num;   //记录数,本页存储的t_item记录数
			uint32_t treeid;//树号，默认0,用于一个页面文件里多个树,可以使用标签的taguid

			typeidxv pgidx;//整个页的起始idx分割值,不随其页面内的记录而变,页面内的所有索引都大于等于这个值。
			typepgno right; //同级右指针,不会增加写盘IO次数
		};//sizeof() = 24

		class page_ //索引页面对象，按照小头优化落地。
		{
		public:
			size_t _pgsize;// 8192 or 16384
			typepgno _pgno; //索引页面号
			t_pghead _h; //页头
			ec::vector<t_item> _items; //索引项,叶子节点的索引项指向数据页面,非叶子节点索引指向下一级索引页面。
		public:
			page_& operator = (const page_& pg)
			{
				_pgno = pg._pgno;
				_h = pg._h;
				_items.clear();
				_items.insert(_items.begin(), pg._items.begin(), pg._items.end());
				return *this;
			}
			inline static size_t size_head() {
				return 8u + sizeof(typeidxv) + sizeof(typepgno);// +sizeof(typepgno);
			}
			inline static size_t size_item() {
				return sizeof(typeidxv) + sizeof(typepgno);
			}

			inline size_t capacity() const
			{
				return (_pgsize - size_head()) / size_item();
			}

			inline void set_treeid(uint32_t treeid)
			{
				_h.treeid = treeid;
			}

			inline uint32_t get_treeid() {
				return _h.treeid;
			}

			page_(size_t pgsize, typepgno pgno = EC_PGF_ENDNO,
				uint16_t pgtype = BPTREE_PAGE_LEAF,
				typeidxv pgidx = _MixIdxv().minidxv())
			{
				_pgsize = pgsize;
				_pgno = pgno;
				_h.flag = pgtype;
				_h.num = 0;
				_h.treeid = 0;
				_h.pgidx = pgidx;

				_h.right = EC_PGF_ENDNO;

				_items.reserve(capacity());
			}

			page_(const page_& v)
			{
				_pgsize = v._pgsize;
				_pgno = v._pgno;
				_h = v._h;
				_items.reserve(capacity());
				if (v._items.size())
					_items.insert(_items.begin(), v._items.begin(), v._items.end());
			}

			inline size_t pagesize()
			{
				return size_head() + size_item() * _items.size();
			}

			/**
			 * @brief 从页面解码
			 * @param page  页面
			 * @return  成功返回 EC_PGF_SUCCESS,其余为错误码
			*/
			int frompage(void* page)
			{
				_items.clear();
				ec::stream ss(page, _pgsize);
				ss >> _h.flag >> _h.num >> _h.treeid >> _h.pgidx >> _h.right;
				if ((BPTREE_PAGE_IDX != _h.flag && BPTREE_PAGE_LEAF != _h.flag)
					|| _h.num > capacity() || !_h.num)
					return EC_PGF_ERR_PAGE;

				t_item vi;
				for (auto i = 0u; i < _h.num; i++) {
					ss >> vi.idxv >> vi.pgno;
					_items.emplace_back(vi.idxv, vi.pgno);
				}
				return BPTREE_SUCCESS;
			}

			/**
			 * @brief 编码到页面
			 * @param page 输出的页面
			 * @return 成功返回 EC_PGF_SUCCESS,其余为错误码
			*/
			int topage(void* page) // 返回<0错误码，>0字节数
			{
				_h.num = (uint16_t)_items.size();
				if ((BPTREE_PAGE_IDX != _h.flag && BPTREE_PAGE_LEAF != _h.flag) || !_h.num)
					return EC_PGF_ERR_PAGE;
				if (_h.num > capacity())
					return EC_PGF_ERR_PAGE;

				ec::stream ss(page, _pgsize);
				ss << _h.flag << _h.num << _h.treeid << _h.pgidx << _h.right;

				for (auto& vi : _items)
					ss << vi.idxv << vi.pgno;
				return (int)ss.getpos();
			}

			/**
			 * @brief 更改 item.idxv的索引页面
			 * @param item 
			 * @return 返回操作状态
			 * @remark 只会用于叶子页面
			*/
			int  updatepgno(const t_item& item)
			{
				for (auto& i : _items) {
					if (i.idxv == item.idxv) {
						if (i.pgno == item.pgno)
							return BPTREE_ITEM_NOTCHANGE;
						i.pgno = item.pgno;
						return BPTREE_ITEM_UPDATED;
					}
				}
				return BPTREE_ITEM_NOTEXIST;
			}

			inline void add(typeidxv idxv, typepgno datapageno)
			{
				_items.emplace_back(idxv, datapageno);
			}

			/**
			 * @brief 插入索引
			 * @param item 
			 * @remark _items里索引项的索引值一定是递增的
			*/
			void insert(const t_item& item)
			{
				if (_items.empty()) {
					_items.push_back(item);
					return;
				}
				//剩下不存在，需要插入
				if (item.idxv > _items.back().idxv) //最后
					_items.push_back(item);
				else if (item.idxv < _items[0].idxv) //最前
					_items.insert(_items.begin(), item);
				else {
					int ipos = bsearch(item.idxv);
					_items.insert(_items.begin() + ipos + 1, item);
				}
			}

			/**
			 * @brief 备份页面,只拷贝有效部分。
			 * @param pgsrc 源页面
			 * @param pgbak 目标页面
			 * @return 返回页面实际字节数
			*/
			size_t bakpg(const void* pgsrc, void* pgbak)
			{
				size_t pgbaksize = pagesize();
				memcpy(pgbak, pgsrc, pgbaksize);
				return pgbaksize;
			}

			/**
			 * @brief 分割页面
			 * @param splitpos 分割位置，splitpos之后的移动到新页面
			 * @param pgnew 新页面
			*/
			void split(size_t splitpos, page_& pgnew)//分离页面
			{
				pgnew._items.insert(pgnew._items.begin(), _items.begin() + splitpos, _items.end()); //新页面在后
				pgnew._h.pgidx = pgnew._items[0].idxv;

				_items.erase(_items.begin() + splitpos, _items.end());//原页面在左面
				pgnew._h.right = _h.right;
				_h.right = pgnew._pgno;
			}

			/**
			 * @brief 二分查找最idxv插入的位置
			 * @param idxv 
			 * @return 返回插入位置，前插入
			*/
			int bsearch(const typeidxv& idxv) //二分查找最idxv所在索引位置
			{
				int n = (int)_items.size();
				if (n < 8) { //直接查找
					int i = n - 1;
					for (; i >= 0; i--) {
						if (idxv >= _items[i].idxv)
							return i;
					}
					return 0;
				}
				int nh = n - 1, nl = 0, nm;
				while (nl <= nh) {
					nm = (nh + nl) / 2;
					if (idxv == _items[nm].idxv)
						return nm;
					else if (_items[nm].idxv < idxv)
						nl = nm + 1;
					else
						nh = nm - 1;
				}
				while (nm > 0 && _items[nm].idxv > idxv)
					nm--;
				return nm;
			}
		};
	protected:
		ipage_storage* _pgstor; //页面存储接口
		/**
		 * @brief  rootpgno在插入和删除过程中会变化，初始化时设置该值，当操作完成后，需要读取该值保存。
		*/
		typepgno _rootpgno;// root页的页面号;EC_PGF_ENDNO表示空, 没有root节点,

		uint32_t _treeid; //树ID，用于标记页面头部的treeid

	protected:
		int readpage(int64_t pgno, page_& pgobj) //从pgno读页面并解析到pgobj
		{
			ec::autobuf<char> page(_pgstor->pg_size());
			char* pgbuf = page.data();
			if (_pgstor->pg_read(pgno, 0, pgbuf, _pgstor->pg_size()) < 0)
				return EC_PGF_ERR_READ;
			pgobj._pgno = pgno;
			return pgobj.frompage(pgbuf);
		}
		int readpage(int64_t pgno, page_& pgobj, void* pgbuf)//从pgno读页面并解析到pgobj
		{
			if (_pgstor->pg_read(pgno, 0, pgbuf, _pgstor->pg_size()) < 0)
				return EC_PGF_ERR_READ;
			pgobj._pgno = pgno;
			return pgobj.frompage(pgbuf);
		}
		int writepage(int64_t pgno, page_& pgobj) //序列化页面并写入pgno页面
		{
			ec::autobuf<char> page(_pgstor->pg_size());
			char* pgbuf = page.data();
			int nsize = pgobj.topage(pgbuf);
			if (nsize <= 0)
				return EC_PGF_ERR_PAGE;
			return _pgstor->pg_write(pgno, 0, pgbuf, nsize) > 0 ? BPTREE_SUCCESS : EC_PGF_ERR_WRITE;
		}

	public:
		btree(ipage_storage* pstor = nullptr,
			typepgno rootpgno = EC_PGF_ENDNO,
			uint32_t treeid = 0)
			: _pgstor(pstor), _rootpgno(rootpgno), _treeid(treeid)
		{

		}

		void set_pagestorage(ipage_storage* pstor, typepgno rootpgno)
		{
			_pgstor = pstor;
			_rootpgno = rootpgno;
		}

		inline typepgno get_rootpgno() {
			return _rootpgno;
		}

		virtual ~btree()
		{
		}

		inline static size_t page_capacity()
		{
			return page_::capacity();
		}

		/**
		 * @brief 插入索引
		 * @param idxv 索引值
		 * @param datapageno 数据页面号 
		 * @param st_result 操作结果 
		 * @return 返回0表示成功,其他为错误码.
		*/
		int insert(typeidxv idxv, typepgno datapageno, int& st_result, ec::ilog* plog = nullptr)
		{
			if (EC_PGF_ENDNO == _rootpgno)
				return createroot(idxv, datapageno);

			int i;
			typepgno pgno = _rootpgno;
			page_ pg(_pgstor->pg_size());
			while (EC_PGF_ENDNO != pgno) {
				if (readpage(pgno, pg) != BPTREE_SUCCESS)
					return BPTREE_FAILED;
				i = pg.bsearch(idxv);
				if (BPTREE_PAGE_LEAF == pg._h.flag)
					return insert_idx(t_item(idxv, datapageno), pgno, st_result, plog);
				if (idxv < pg._items[i].idxv)
					break;
				pgno = pg._items[i].pgno;
			}
			return BPTREE_FAILED;
		}

		/**
		 * @brief 查找idxv最接近的数据页面号
		 * @param idxv 索引值
		 * @param poutpgno [out]查找到的数据页面号
		 * @param poutidxv [out]查找到的数据页面开始的索引值,一般会小于等于idxv
		 * @return 返回0表示成功,其他为错误码.
		 * @remark 成功返回后，应用层根据poutidxv的值决定是否使用数据页面的连接功能读取数据
		*/
		int find(const typeidxv& idxv, typeidxv* poutidx, typepgno* poutpgno)
		{
			int i;
			typepgno pgno = _rootpgno;
			page_ pg(_pgstor->pg_size());
			while (EC_PGF_ENDNO != pgno) {
				if (readpage(pgno, pg) != BPTREE_SUCCESS)
					return BPTREE_FAILED;
				i = pg.bsearch(idxv);
				if (BPTREE_PAGE_LEAF == pg._h.flag) {
					if (poutidx)
						*poutidx = pg._items[i].idxv;
					if (poutpgno)
						*poutpgno = pg._items[i].pgno;
					return BPTREE_SUCCESS;
				}
				if (idxv < pg._items[i].idxv)
					break;
				pgno = pg._items[i].pgno;
			}
			return BPTREE_FAILED;
		}

		/**
		 * @brief 精准删除数据索引
		 * @param idxv 索引值
		 * @param pgno 数据页面号
		 * @return 返回0表示成功,其他为错误码。
		 * @remark 找到索引所在叶子页面，删除记录, 记录不存在当作删除失败处理。
		*/
		int erease(const typeidxv& idxv, typepgno pgno) //删除索引
		{
			t_item it(idxv, pgno);
			return del_idx_(it, BPTREE_PAGE_LEAF);
		}

		/*!
		\brief 递归遍历并打印整颗树
		*/
		void print()
		{
			int lev = -1;
			int nerr = print_(_rootpgno, lev);
			if (BPTREE_SUCCESS != nerr)
				printf("print_tree error %d\n", nerr);
		}

		/**
		 * @brief 清除整个树
		 * @param fun 回调函数
		*/
		inline void clear(std::function<void(typeidxv idxv, typepgno idxpgno)> fun)
		{
			clear_(_rootpgno, fun);
			_rootpgno = EC_PGF_ENDNO;
		}

		/**
		 * @brief 遍历所有叶子节点索引
		 * @param fun 回调函数
		*/
		inline void foreach(std::function<void(typeidxv idxv, typepgno idxpgno)> fun)
		{
			foreach_(_rootpgno, fun);
		}
	protected:
		/**
		 * @brief 创建一个树, 并将(idxv,pgno)加入其中
		 * @param idxv 索引值
		 * @param pgno 索引数据页面
		 * @return 0:success; -1:error
		*/
		int createroot(typeidxv idxv, typepgno pgno)
		{
			typepgno rootpgno = (typepgno)_pgstor->pg_alloc();
			if (EC_PGF_ENDNO == rootpgno)
				return EC_PGF_ERR_ALLOC;
			page_ pg(_pgstor->pg_size(), pgno, BPTREE_PAGE_LEAF, _MixIdxv().minidxv());//第一个节点从最小索引值开始，为数据页面。
			pg.add(idxv, pgno);

			int nst;
			if ((nst = writepage(rootpgno, pg)) != BPTREE_SUCCESS) {
				_pgstor->pg_free(rootpgno);
				rootpgno = EC_PGF_ENDNO;
			}
			_rootpgno = rootpgno;
			return nst;
		}

		/**
		 * @brief 将item插入或替换到pgno中，如果满则递归向上分裂,如果存在idxv,将会更改pgno
		 * @param item 待插入的索引记录
		 * @param pgno 插入的索引页面
		 * @param result 操作结果,BPTREE_ITEM_INSERTED,BPTREE_ITEM_NOTCHANGE,BPTREE_ITEM_UPDATED之一
		 * @return 返回0表示成功，其他为错误码。
		 * @remark pgno页记录不为空， pitem->idxv >= pgno的pgidx
		*/
		int insert_idx(const t_item& item, typepgno pgno, int& result, ec::ilog* plog = nullptr)
		{
			assert(pgno != EC_PGF_ENDNO);
			result = BPTREE_ITEM_INSERTED;//默认insert;
			int nst;
			page_ pg(_pgstor->pg_size());
			ec::autobuf<char> page(_pgstor->pg_size());
			char* pgbuf = page.data();

			if ((nst = readpage(pgno, pg, pgbuf)) != BPTREE_SUCCESS)
				return nst;
			size_t pgsize = pg.pagesize();//备份页面
			nst = pg.updatepgno(item); //先尝试更改替换页面号
			if (BPTREE_ITEM_NOTCHANGE == nst) {
				result = BPTREE_ITEM_NOTCHANGE; //这种场景也不可能发生.
				return BPTREE_SUCCESS;
			}
			else if (BPTREE_ITEM_UPDATED == nst) { //更新后重新写盘
				result = BPTREE_ITEM_UPDATED; //所有数据的idxv不同，因此不会产生 更新索引场景。
				return writepage(pgno, pg);
			}

			pg.insert(item);
			if (pg._items.size() + 1 < pg.capacity()) { //无需分裂
				if (plog) {
					plog->add(CLOG_DEFAULT_DBG, "pgidx itemsze=%zu,pgcapacity=%zu", pg._items.size(), pg.capacity());
				}
				return writepage(pgno, pg);
			}

			typepgno pgno_new = (typepgno)_pgstor->pg_alloc();//下面开始分裂
			if (EC_PGF_ENDNO == pgno_new)
				return EC_PGF_ERR_ALLOC;

			page_ pgnew(_pgstor->pg_size(), pgno_new, pg._h.flag);
			pgnew.set_treeid(_treeid);
			pg.split(pg._items.size() / 2, pgnew);//默认中分页面
			if (writepage(pgno_new, pgnew) != BPTREE_SUCCESS
				|| writepage(pgno, pg) != BPTREE_SUCCESS
				) {
				_pgstor->pg_free(pgno_new);
				_pgstor->pg_write(pgno, 0, pgbuf, pgsize); //恢复原页面。
				return EC_PGF_ERR_WRITE;
			}
			nst = BPTREE_SUCCESS;
			do {
				if (pgno == _rootpgno) { //顶部，升级树
					typepgno newrootpgno = (typepgno)_pgstor->pg_alloc();
					if (EC_PGF_ENDNO == newrootpgno) {
						nst = EC_PGF_ERR_ALLOC;
						break;
					}
					page_ pgroot(_pgstor->pg_size(), newrootpgno, BPTREE_PAGE_IDX, pg._h.pgidx);
					pgroot.set_treeid(_treeid);
					pgroot.add(pg._h.pgidx, pgno);
					pgroot.add(pgnew._h.pgidx, pgno_new);
					if ((nst = writepage(newrootpgno, pgroot)) != BPTREE_SUCCESS) {
						_pgstor->pg_free(newrootpgno);
						break;
					}
					_rootpgno = newrootpgno;
					break;
				}

				int ipos = 0;
				page_ pgup(_pgstor->pg_size());
				if (BPTREE_SUCCESS != (nst = find_in_page_(_rootpgno, t_item(pg._h.pgidx, pgno), pgup, ipos)))
					break;
				nst = insert_idx(t_item(pgnew._h.pgidx, pgno_new), pgup._pgno, result, plog);
			} while (0);
			if (BPTREE_SUCCESS != nst) {
				_pgstor->pg_free(pgno_new);
				_pgstor->pg_write(pgno, 0, pgbuf, pgsize);  //恢复原页面。
			}
			return nst;
		}

		/*!
		\brief 删除pgno页面里的 idel索引记录
		\param idel要删除的索引记录
		\return 返回0成功，其他为错误码.
		\remark 删除后如果需要合并，则合并后继续删除被合并掉的页面记录，是一个递归调用，深度小于树高.
		*/
		int del_idx_(const t_item& idel, uint16_t pagetype = BPTREE_PAGE_IDX)
		{
			int nst = 0, ipos = 0;
			page_ pg(_pgstor->pg_size());
			if (BPTREE_SUCCESS != (nst = find_in_page_(_rootpgno, idel, pg, ipos, pagetype))) {
				return nst;
			}

			pg._items.erase(pg._items.begin() + ipos);
			if (pg._items.size() > pg.capacity() / 4)//减少IO次数，大于1/4不合并
				return writepage(pg._pgno, pg);

			t_item idelup;
			if (pg._h.right != EC_PGF_ENDNO) {//将右合并过来
				if ((nst = mergeright_(pg, idelup)) == BPTREE_SUCCESS) {
					nst = del_idx_(idelup);
					if (nst) {
						tracetree("mergeright del_idx_(%lld, pg %lld) = %d\n",
							idelup.idxv, idelup.pgno, nst);
					}
					return nst;
				}
				if (EC_PGF_ERR_WRITE == nst)
					return nst;
			}

			if (mergeleft_(pg, idelup) == BPTREE_SUCCESS) { //合并到左边
				nst = del_idx_(idelup);
				if (nst) {
					tracetree("mergeleft del_idx_(%lld, pg %lld) = %d\n",
						idelup.idxv, idelup.pgno, nst);
				}
				return nst;
			}

			if (!pg._items.empty()) {
				//tracetree("WRN: writepage page %lld, items low limit %zu\n", pg._pgno, pg._items.size());
				return writepage(pg._pgno, pg);
			}

			_pgstor->pg_free(pg._pgno);
			nst = BPTREE_SUCCESS;
			if (pg._pgno == _rootpgno) {
				_rootpgno = EC_PGF_ENDNO;
				tracetree("delete root page %lld\n", pg._pgno);
			}
			else {
				idelup.idxv = pg._h.pgidx;
				idelup.pgno = pg._pgno;
				nst = del_idx_(idelup);
				tracetree("isolated del_idx_(%lld, pg %lld) = %d\n",
					idelup.idxv, idelup.pgno, nst);
			}
			return nst;
		}

		int print_(typepgno pgno, int& level)//递归遍历并打印整颗树
		{
			if (EC_PGF_ENDNO == pgno) {
				printf("***empty\n");
				return BPTREE_SUCCESS;
			}
			int nst;
			page_ pg(_pgstor->pg_size());
			if ((nst = readpage(pgno, pg)) != BPTREE_SUCCESS)
				return nst;
			level++;
			std::string stab(level * 4, '\x20');
			printf("%s", stab.c_str());

			std::string stitle;
			if (pgno == _rootpgno)
				stitle = "ROOT";
			if (pg._h.flag == BPTREE_PAGE_LEAF) {
				stitle += "LEAF";
				printf("L%d %s pgno(%lld): pgidv(%lld), right(%lld), items(%zu)\n", level, stitle.c_str(),
					(long long)pgno, (long long)pg._h.pgidx, (long long)pg._h.right, pg._items.size());
				stab += "    ";
				for (auto& i : pg._items)
					printf("%s(%lld:%lld)\n", stab.c_str(), (long long)i.idxv, (long long)i.pgno);
			}
			else {
				stitle += "_IDX";
				printf("L%d %s pgno(%lld): pgidv(%lld), right(%lld), items(%zu)\n", level, stitle.c_str(),
					(long long)pgno, (long long)pg._h.pgidx, (long long)pg._h.right, pg._items.size());
				for (auto& i : pg._items) {
					printf("%s[%lld : %lld]\n", stab.c_str(), (long long)i.idxv, (long long)i.pgno);
					print_(i.pgno, level);
				}
			}
			level--;
			return BPTREE_SUCCESS;
		}

		/**
		 * @brief 遍历所有数据节点
		 * @param pgno 索引页面号，第一次为root页面号
		 * @param fun 
		 * @return 成功返回BPTREE_SUCCESS ,失败返回错误码
		*/
		int foreach_(typepgno pgno, std::function<void(typeidxv idxv, typepgno idxpgno)> fun)
		{
			if (EC_PGF_ENDNO == pgno) {
				return BPTREE_SUCCESS;
			}
			int nst;
			page_ pg(_pgstor->pg_size());
			if ((nst = readpage(pgno, pg)) != BPTREE_SUCCESS)
				return nst;

			if (pg._h.flag == BPTREE_PAGE_LEAF) {
				for (auto& i : pg._items)
					fun(i.idxv, i.pgno);
			}
			else {
				for (auto& i : pg._items) {
					foreach_(i.pgno, fun);
				}
			}
			return BPTREE_SUCCESS;
		}

		/**
		 * @brief 删除整个索引树
		 * @param pgno 
		 * @param fun 叶子节点数据页面删除时回调处理
		 * @return 0:success; 其他错误码
		*/
		int clear_(typepgno pgno, std::function<void(typeidxv idxv, typepgno idxpgno)> fun) //清理以pgno为根的树
		{
			if (EC_PGF_ENDNO == pgno) {
				printf("***end\n");
				return BPTREE_SUCCESS;
			}
			int nst;
			page_ pg(_pgstor->pg_size());
			if ((nst = readpage(pgno, pg)) != BPTREE_SUCCESS)
				return nst;

			if (pg._h.flag == BPTREE_PAGE_LEAF) {
				for (auto& i : pg._items)
					fun(i.idxv, i.pgno);
				if (!_pgstor->pg_free(pg._pgno)) {
					tracetree("ERR: LEAF pg_free pgno %lld error\n", pg._pgno);
				}
			}
			else {
				for (auto& i : pg._items) {
					clear_(i.pgno, fun);
				}
				if (!_pgstor->pg_free(pg._pgno)) {
					tracetree("ERR: LEAF pg_free pgno %lld error\n", pg._pgno);
				}
			}
			return BPTREE_SUCCESS;
		}

	private:
		//从pstart开始开始寻找pnode所在页面, 并将页面输出到pgin,页面号输出到 pginno
		int find_in_page_(typepgno pgnostart, const t_item& node, page_& pgin, int& ipos, uint16_t pagetype = BPTREE_PAGE_IDX)
		{
			int i;
			typepgno pgno = pgnostart;
			while (EC_PGF_ENDNO != pgno) {
				if (readpage(pgno, pgin) != BPTREE_SUCCESS || pgin._items.empty())
					break;
				i = pgin.bsearch(node.idxv);
				if (node.idxv == pgin._items[i].idxv && node.pgno == pgin._items[i].pgno && pgin._h.flag == pagetype) {
					ipos = i;
					return BPTREE_SUCCESS;
				}
				else if (node.idxv < pgin._items[i].idxv || BPTREE_PAGE_LEAF == pgin._h.flag)
					break;
				pgno = pgin._items[i].pgno;
			}
			return BPTREE_FAILED;
		}

		//查找叶子索引,成功后,pg存储的叶子节点，ipos存储的叶子节点页面号
		int find_leaf_idx_(const typeidxv& idxv, page_& pg, int* ipos = nullptr)
		{
			int i;
			typepgno pgno = _rootpgno;
			while (EC_PGF_ENDNO != pgno) {
				if (readpage(pgno, pg) != BPTREE_SUCCESS)
					return BPTREE_FAILED;
				i = pg.bsearch(idxv);
				if (BPTREE_PAGE_LEAF == pg._h.flag) {
					if (pg._items[i].idxv == idxv) {
						if (ipos)
							*ipos = i;
						return BPTREE_SUCCESS;
					}
					break;
				}
				if (idxv < pg._items[i].idxv)
					break;
				pgno = pg._items[i].pgno;
			}
			return BPTREE_FAILED;
		}

		int find_left(const t_item& node, t_item& left) //找同根左节点
		{
			int ipos = 0;
			page_ pg(_pgstor->pg_size());
			if (BPTREE_SUCCESS != find_in_page_(_rootpgno, node, pg, ipos) || !ipos)
				return BPTREE_FAILED;
			left = pg._items[ipos - 1];
			return BPTREE_SUCCESS;
		}

		int find_right(const t_item& node, t_item& right)//找同根右点
		{
			int ipos = 0;
			page_ pg(_pgstor->pg_size());
			if (BPTREE_SUCCESS != find_in_page_(_rootpgno, node, pg, ipos)
				|| ipos + 1u >= pg._items.size())
				return BPTREE_FAILED;
			right = pg._items[ipos + 1];
			return BPTREE_SUCCESS;
		}

		int mergeright_(const page_& pg, t_item& idel) //将右边合并过来
		{
			if (pg._h.right == EC_PGF_ENDNO)
				return BPTREE_FAILED;

			t_item node(pg._h.pgidx, pg._pgno), noderight;
			if (BPTREE_SUCCESS != find_right(node, noderight))
				return BPTREE_FAILED;

			page_ pgr(_pgstor->pg_size());
			if (readpage(pg._h.right, pgr) != BPTREE_SUCCESS)
				return BPTREE_FAILED;
			if (pgr._items.size() + pg._items.size() < pg.capacity()) {
				page_ pgn(pg);
				idel.idxv = pgr._h.pgidx;
				idel.pgno = pgr._pgno;
				pgn._items.insert(pgn._items.end(), pgr._items.begin(), pgr._items.end());
				pgn._h.right = pgr._h.right;

				if (BPTREE_SUCCESS != writepage(pgn._pgno, pgn)) {
					tracetree("mergeright_ pgno = %lld, pgnoright = %lld write error \n",
						pgn._pgno, pgr._pgno);
					return EC_PGF_ERR_WRITE;
				}
				_pgstor->pg_free(idel.pgno);
				return BPTREE_SUCCESS;
			}
			tracetree("mergeright_ pgno = %lld, pgnoright = %lld full,size = %zu + right size %zu \n",
				pg._pgno, pg._h.right, pg._items.size(), pgr._items.size());
			return 	BPTREE_FAILED;
		}

		int mergeleft_(const page_& pg, t_item& idel)//合并到左边
		{
			t_item node(pg._h.pgidx, pg._pgno), nodeleft;
			if (BPTREE_SUCCESS != find_left(node, nodeleft))
				return BPTREE_FAILED;

			page_ pgl(_pgstor->pg_size());
			if (readpage(nodeleft.pgno, pgl) != BPTREE_SUCCESS)
				return BPTREE_FAILED;
			if (pgl._items.size() + pg._items.size() < pg.capacity()) {
				idel.idxv = pg._h.pgidx;
				idel.pgno = pg._pgno;
				pgl._items.insert(pgl._items.end(), pg._items.begin(), pg._items.end());
				pgl._h.right = pg._h.right;

				if (BPTREE_SUCCESS != writepage(pgl._pgno, pgl)) {
					tracetree("ERR: mergeleft_ pgno = %lld, pgnoleft = %lld write error \n",
						pg._pgno, pgl._pgno);
					return EC_PGF_ERR_WRITE;
				}
				_pgstor->pg_free(idel.pgno);
				return BPTREE_SUCCESS;
			}
			tracetree("mergeleft_ pgno = %lld, pgnoleft = %lld full,size = %zu + left size %zu \n",
				pg._pgno, pgl._pgno, pg._items.size(), pgl._items.size());
			return 	BPTREE_FAILED;
		}
	};// btree
}// namespace ec