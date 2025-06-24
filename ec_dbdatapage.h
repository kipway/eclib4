/*!
\brief rdb_objpage.h
object save page

\author	jiangyong
\email  kipway@outlook.com
\update
  2023.10.25 优化 CDataPage::Insert()，更新注释

eclib 3.0 Copyright (c) 2017-2023, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once

#include "ec_vector.hpp"
#include "ec_crc.h"
#include "ec_stream.h"
#include "ec_protoc.h"

#ifndef RDB_DATA_TBS_FILEKIOLPAGES
#if defined(_ARM_LINUX) || defined(_MEM_TINY) || defined(_MEM_SML)
#define RDB_DATA_TBS_FILEKIOLPAGES 32 //数据表空间的单个文件页面KIOL数, 32 * 1024 * 8192 = 256M
#else
#define RDB_DATA_TBS_FILEKIOLPAGES 255 //数据表空间的单个文件页面KIOL数, 255 * 1024 * 8192 = 2G - 8M
#endif
#endif

namespace ec
{
	constexpr int RDB_DATAPAGE_HEAD_SIZE = 40; //页面头大小
	constexpr int RDB_DATAPAGE_INSERT_RES_SIZE = 128;  //插入时页面保留空间大小
	constexpr int RDB_DATAPAGE_MAX_NUMOBJS = 65535;//页面最大对象数
	constexpr int RDB_DATAPAGE_MAX_DATASIZE = (65535 - 40);//页面最大编码后字节数
	/*
	  页面头部信息
	  双向列表
	*/
	class CDbPageHead
	{
	public:
		uint16_t _flag;//魔数
		uint16_t _ver; //版本,1000
		uint16_t _size; //页面序列化后的数据大小，不包含页面头。页面必须小于64K
		uint16_t _numrecs; //记录数
		int64_t _idxval; //该页面的起始索引值, 初始化后不会被修改，删除页面时可以同步从索引中删除.也可以用于重建索引
		int64_t _prevpgno; //上一页,-1表示字节是头
		int64_t _nextpgno; //下一页,-1表示结束
		uint32_t _objid; //对象IO，唯一，一般用于TagID
		uint32_t _crc32head; //前面序列化后一共36字节的CRC32校验值。
	public:
		CDbPageHead() :_flag(0), _ver(1000)
			, _size(0)
			, _numrecs(0)
			, _idxval(0)
			, _prevpgno(-1)
			, _nextpgno(-1)
			, _objid(0)
			, _crc32head(0)
		{
		}

		int tobuf(void* buf, uint16_t flag)// return >0 size; -1:error
		{
			ec::stream ss(buf, RDB_DATAPAGE_HEAD_SIZE);
			try {
				_flag = flag;
				ss << _flag << _ver << _size << _numrecs << _idxval << _prevpgno << _nextpgno << _objid;
				_crc32head = ec::crc32(buf, (uint32_t)ss.getpos());
				ss << _crc32head;
			}
			catch (...) {
				return -1;
			}
			return (int)ss.getpos();
		}

		int frombuf(void* buf, uint16_t flag)// return 0:success; -1:error
		{
			ec::stream ss(buf, RDB_DATAPAGE_HEAD_SIZE);
			try {
				ss >> _flag >> _ver >> _size >> _numrecs >> _idxval >> _prevpgno >> _nextpgno >> _objid;
				uint32_t chkv = ec::crc32(buf, (uint32_t)ss.getpos());
				ss >> _crc32head;
				if (chkv != _crc32head || _flag != flag)
					return -1;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};

	/*
	  存储标签历史数据对象的页面,使用时标作为索引
	  技巧,第一个页面的索引_key为0不变，最小。如果不删除，永远不会发生修改索引情况。
	  在任何一个页面插入记录如果发生分页，插入位置在前半部分在1/3处分页，后半部分在2/3处分页。
	  发生分页后，需要做一次缓冲写盘和提交索引操作。
	  一个模板类适合存储标签值，标签对象，SOE记录集
	*/
	template<class _OBJ> // _OBJ = pgo_tagval
	class CDbDataPage
	{
	public:
		enum pg_operate {
			pgopt_none = 0,
			pgopt_update = 1,
			pgopt_insert = 2
		};

	public:
		CDbPageHead _head; //页面头
		ec::vector<_OBJ> _objs; // 记录集
	public:
		void clear() {
		};
	public:
		CDbDataPage() {
			_objs.reserve(1024);
		}

		/*
		* brief 将obj插入页面对象集合
		* param obj 插入的对象
		* return 0:无变更;  1:update; 2:insert; -1:error
		*/
		int Insert(const _OBJ& obj)
		{
			int64_t lkey = obj.get_idxval(), kt;
			if (_objs.empty()) { //空优化
				_objs.push_back(obj);
				return 2;
			}
			//先尾部追加优化
			kt = _objs.back().get_idxval();
			if (lkey > kt) {
				_objs.push_back(obj);
				return 2;
			}
			else if (lkey == kt) {
				if (_objs.back().equal(obj))//内容相同直接返回
					return 0;
				_objs.back() = obj;
				return 1;
			}
			//再小集合直接遍历插入
			if (_objs.size() < 10) {
				for (auto i = _objs.begin(); i != _objs.end(); i++) {
					kt = i->get_idxval();
					if (kt == lkey) { //idx相同覆盖
						if (i->equal(obj))//内容相同直接返回
							return 0;
						(*i) = obj;//覆盖
						return 1;
					}
					else if (kt > lkey) {
						_objs.insert(i, obj);//在i之前
						return 2;
					}
				}
			}
			else { //二分查找寻找插入点插入
				int nl = 0, nh = (int)_objs.size() - 1, nm = (nl + nh) / 2;
				while (nl <= nh) { //二分查找替换
					nm = (nl + nh) / 2;
					if (_objs[nm].get_idxval() == lkey) {
						if(_objs[nm].equal(obj))
							return 0; //内容相同直接返回
						_objs[nm] = obj;
						return 1;
					}
					else if (_objs[nm].get_idxval() < lkey) {
						nl = nm + 1;
					}
					else {
						nh = nm - 1;
					}
				}
				if (_objs[nm].get_idxval() > lkey) {
					_objs.insert(_objs.begin() + nm, obj);//在nm之前
				}
				else {
					_objs.insert(_objs.begin() + nm + 1, obj);//在nm之后
				}
				return 2;
			}
			return -1;
		}

		/*
		* brief 分裂页面, 递增按照3/4处分，递减 1/4处分.
		* param pg2rd输出的第二个页面的对象集合
		* param pgsize 页面大小比如8192
		* param binc 是否递增插入
		* return 0:没有分页; >0 分页后第一个页面剩下的记录数;
		*/
		int SplitPage(ec::vector<_OBJ>& pg2rd, size_t pgsize, bool binc) //分页,按照一半大小分页, return 0: 没有分; >0 剩下的记录数;
		{
			size_t zl = 0, zsp = binc ? (pgsize / 2 + pgsize / 4) : pgsize / 4; //递增按照3/4处分，递减 1/4处分.
			uint32_t fid = _OBJ::get_field_number();//获取对象的PB3编码 field number
			int i = 0, n = (int)_objs.size();
			_OBJ* p = _objs.data();
			for (; i < n; i++) { //逐个计算编码后的大小,到达分离点zsp后分离页面
				if (!i)
					zl += p[i].size_z(fid, nullptr);
				else
					zl += p[i].size_z(fid, p + i - 1);
				if (zl >= zsp || i == n - 1) {
					if (i + 1 < n) {
						pg2rd.insert(pg2rd.end(), std::make_move_iterator(_objs.begin() + i + 1), std::make_move_iterator(_objs.end()));
						_objs.resize(i + 1);
						return i + 1;
					}
					return 0;
				}
			}
			return 0;
		}

		/**
		 * @brief 计算编码后字节数，不含页面头部
		 * @return 
		*/
		size_t SizeEncode()
		{
			int fid = _OBJ::get_field_number(), n = (int)_objs.size();
			size_t z = 0;
			_OBJ* p = _objs.data();
			for (auto i = 0; i < n; i++) {
				if (!i)
					z += p[i].size_z(fid, nullptr);
				else
					z += p[i].size_z(fid, p + i - 1);
			}
			return z;
		}

		/**
		 * @brief 页面输出到pvo并改写_head的size和_numrecs,不含头部
		 * @param pvo 追加模式的输出流.
		 * @return 0
		*/
		int OutPage(ec::bytes* pvo)
		{
			int fid = _OBJ::get_field_number();
			int n = (int)_objs.size();
			_OBJ* p = _objs.data();
			size_t zlen = pvo->size();
			for (auto i = 0; i < n; i++) {
				if (!i)
					p[i].out_z(fid, pvo, nullptr);
				else
					p[i].out_z(fid, pvo, p + i - 1);
			}
			_head._size = static_cast<uint16_t>(pvo->size() - zlen);
			_head._numrecs = static_cast<uint16_t>(_objs.size());
			return 0;
		}

		/**
		 * @brief 从页面恢复到记录集
		 * @param pbytes 页面数据,纯数据区
		 * @param size 页面数据长度,不含头部.
		 * @return 0:success; -1:error
		*/
		int FromPage(const void* pbytes, size_t size)
		{
			_objs.clear();
			if (!ec::pb::parse(pbytes, size, *this))
				return -1;
			int n = (int)_objs.size();
			_OBJ* po = _objs.data();
			for (auto i = 1; i < n; i++)
				po[i].restore(po + i - 1);
			return 0;
		}

		void on_var(uint32_t field_number, uint64_t val) {}
		void on_fixed(uint32_t field_number, const void* pval, size_t size) {}
		void on_cls(uint32_t field_number, const void* pdata, size_t size)
		{
			if (_OBJ::get_field_number() == field_number) {
				_OBJ t;
				if (ec::pb::parse(pdata, size, t))
					_objs.emplace_back(std::move(t));
			}
		}
	};// objspage
} //namespace rdb
