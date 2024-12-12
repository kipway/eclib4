/*!
\file ec_vector.h
\author	jiangyong
\email  kipway@outlook.com
\update
  2024.12.12 pushval参数改为引用
  2024.12.6 增加单一pop删除函数
  2024.5.22 增加遍历函数,增加复制pushval()

ec::lckfree_list

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include <atomic>
#include <functional>
#include "ec_event.h"

namespace ec
{
	/*!
	\breif 一个生成者一个消费者的无锁队列,生产者只push，消费者只pop,类似于单向管道,常用于网络IO线程和业务处理工作线程之间传递消息。
	两个线程之间如果要双向传递消息，可建立两个无锁队列。
	*/
	template < class _Ty>
	class lckfree_list
	{
	public:
		typedef _Ty		value_type;
		typedef size_t	size_type;
		struct t_node {
			t_node* pnext = nullptr;
			value_type  value;
			_USE_EC_OBJ_ALLOCATOR
		};
		lckfree_list(ec::cEvent* pevt = nullptr) : _pevt(pevt), _size(0) {
			_nulnode.pnext = nullptr;
			_phead = &_nulnode;
			_ptail = &_nulnode;
		}
		~lckfree_list() {
			while (_phead) {
				t_node* pre = _phead;
				_phead = _phead->pnext;
				if (pre != &_nulnode)
					delete pre;
			}
			_nulnode.pnext = nullptr;
			_phead = &_nulnode;
			_ptail = &_nulnode;
		}
	public:
		ec::cEvent* _pevt;//触发事件
	protected:
		t_node _nulnode;//空节点,辅助用
		t_node* _phead; //头,其value不可用，消费者从head取出数据。
		t_node* _ptail; //尾,其value不可用，生产者从tail压入数据。
		std::atomic_int _size;
	public:
		void push(value_type&& val)
		{
			t_node* pnew = new t_node;
			pnew->value = std::move(val);
			_ptail->pnext = pnew;
			_ptail = pnew;
			_size++;
			if (_pevt)
				_pevt->SetEvent();
		}
		void pushval(value_type& val)
		{
			t_node* pnew = new t_node;
			pnew->value = val;
			_ptail->pnext = pnew;
			_ptail = pnew;
			_size++;
			if (_pevt)
				_pevt->SetEvent();
		}
		bool pop(value_type& val)
		{
			if (!_phead->pnext)
				return false;
			t_node* pre = _phead;
			_phead = _phead->pnext;
			val = std::move(_phead->value);
			if (pre != &_nulnode)
				delete pre;
			_size--;
			return true;
		}
		bool pop() {
			if (!_phead->pnext)
				return false;
			t_node* pre = _phead;
			_phead = _phead->pnext;
			if (pre != &_nulnode)
				delete pre;
			_size--;
			return true;
		}
		/**
		 * @brief 遍历
		 * @param fun return 0:continue; -1:break;
		 */
		void for_each(std::function<int(value_type& val)>fun)
		{
			t_node* p = _phead;
			while (p->pnext) {
				p = p->pnext;
				if (fun(p->value))
					break;
			}
		}
		inline bool empty() {
			return _phead == _ptail;
		}

		inline int size() {
			return _size;
		}
	};
}//namespace ec
