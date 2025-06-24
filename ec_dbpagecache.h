/*!
*\file rdb_wpgcache.h
* 数据页面缓存

\author	jiangyong
\email  kipway@outlook.com
\update
  2024.11.11 support no ec_alloctor

*/
#pragma once
#include "ec_log.h"
#include "ec_tbs.h"

namespace ec
{
#ifdef _ARM_LINUX
	constexpr size_t DB_WPG_SIZE = 16; //写缓存页面数
#else
	constexpr size_t DB_WPG_SIZE = 32; //写缓存页面数
#endif

	class CPageCache
	{
	public:
		struct t_pgnode {
			int64_t pgno;//页面号
			size_t updatesize; //更新长度, 0表示没有更新
			t_pgnode* pprev; //指向前一个，nullptr表示结束
			t_pgnode* pnext; //指向下一个，nullptr表示结束
			uint8_t page[0]; //页面
		};
	protected:
		ec::tablespace* _ptbs;
		t_pgnode* _phead;//节点头
	public:
		CPageCache(ec::tablespace* ptbs) : _ptbs(ptbs), _phead(nullptr)
		{
		}

		void clear()
		{
			t_pgnode* pg = _phead, * pr;
			while (pg) {
				pr = pg;
				pg = pg->pnext;
				ec::g_free(pr);
			}
			_phead = nullptr;
		}

		virtual ~CPageCache() {
			clear();
		}

		/*!
		* brief 获取页面,并将页面至于头部
		* param pgno 页面号
		* return 返回页面指针
		*/
		uint8_t* GetPage(int64_t pgno)
		{
			t_pgnode* pr = GetPageNode(pgno);
			return  nullptr == pr ? nullptr : pr->page;
		}

		/*!
		* brief 写页面到缓存,并置页面已更新
		* parma pgno 页面号
		* param offset距页面开始的偏移
		* param pbuf 数据
		* param wsize pbuf中需要写入的的字节数
		* return 0：succeed; -1:error
		*/
		int WritePage(int64_t pgno, size_t offset, void* pbuf, size_t wsize)
		{
			if (offset + wsize > _ptbs->SizePage())
				return -1;
			t_pgnode* pgnode = GetPageNode(pgno);
			if (!pgnode)
				return -1;
			if (pgnode->updatesize < offset + wsize)
				pgnode->updatesize = offset + wsize;
			memcpy(pgnode->page + offset, pbuf, wsize);
			return 0;
		}

		//所有页面刷新到磁盘,返回写页面的错误数。
		int FlushAll()
		{
			t_pgnode* pg = _phead;
			int nerr = 0;
			while (pg) {
				if (pg->updatesize) {
					if (_ptbs->writepage(pg->pgno, 0, pg->page, pg->updatesize) < 0)
						nerr++;
					else
						pg->updatesize = 0;
				}
				pg = pg->pnext;
			}
			return nerr;
		}

		//将单个页面刷到磁盘
		int Flush(int64_t pgno)
		{
			t_pgnode* pg = _phead;
			while (pg) {
				if (pg->pgno == pgno && pg->updatesize) {
					if (_ptbs->writepage(pg->pgno, 0, pg->page, pg->updatesize) < 0)
						return -1;
					pg->updatesize = 0;
					break;
				}
				pg = pg->pnext;
			}
			return 0;
		}

		//仅从缓存删除页面, 用于失败后恢复
		int RemovePage(int64_t pgno)
		{
			t_pgnode* pg = _phead;
			while (pg) {
				if (pg->pgno == pgno) {//存在
					if (pg == _phead) {
						if (pg->pnext) {
							_phead = pg->pnext;
							_phead->pprev = nullptr;
						}
						else
							_phead = nullptr;
					}
					else {
						pg->pprev->pnext = pg->pnext;
						if (pg->pnext)
							pg->pnext->pprev = pg->pprev;
					}
					ec::g_free(pg);
					return 0;
				}
				pg = pg->pnext;
			}
			return -1;
		}
	protected:

		/*!
		* brief 获取页面节点,并将页面至于头部
		* param pgno 页面号
		* return 返回页面节点指针
		*/
		t_pgnode* GetPageNode(int64_t pgno)
		{
			t_pgnode* pg = _phead, * pr = nullptr;
			size_t num = 0;
			while (pg) {
				if (pg->pgno == pgno) {//存在
					if (pg->pprev) { //非头,摘除后放到头部.
						pg->pprev->pnext = pg->pnext;
						if (pg->pnext)
							pg->pnext->pprev = pg->pprev;
						pg->pprev = nullptr;
						pg->pnext = _phead;
						_phead = pg;
					}
					return pg;
				}
				num++;
				pr = pg;
				pg = pg->pnext;
			}
			if (num >= DB_WPG_SIZE) { //满,重用最后一个页面pr
				if (pr->updatesize) { //页面已更新，需要写回磁盘。
					if (_ptbs->writepage(pr->pgno, 0, pr->page, pr->updatesize) < 0)
						return nullptr;
					pr->updatesize = 0;
				}
				pr->pprev->pnext = nullptr; //摘除这个页面
				pr->pgno = pgno;
				pr->updatesize = 0;
				if (_ptbs->readpage(pr->pgno, 0, pr->page, _ptbs->SizePage()) < 0) { //读页面错误
					ec::g_free(pr);
					return nullptr;
				}
				pr->pprev = nullptr;
				pr->pnext = _phead;
				_phead->pprev = pr;
				_phead = pr;
				return pr;
			}
			//下面处理没有满,直接分配一个
			pg = (t_pgnode*)ec::g_malloc(sizeof(t_pgnode) + _ptbs->SizePage());
			if (!pg)
				return nullptr;
			pg->pgno = pgno;
			pg->updatesize = 0;
			if (_ptbs->readpage(pg->pgno, 0, pg->page, _ptbs->SizePage()) < 0) { //读页面错误
				ec::g_free(pg);
				return nullptr;
			}
			//pg插入头部成为头部
			pg->pprev = nullptr;
			pg->pnext = _phead;
			if (_phead)
				_phead->pprev = pg;
			_phead = pg;
			return pg;
		}
	};
}// namespace rdb
