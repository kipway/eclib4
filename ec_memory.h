﻿/*!
\file ec_memory.h
\author	jiangyong
\email  kipway@outlook.com
\update
  2024-12-05 add io_buffer::setsizemax() and parsebuffer::bufsize()
  2024-12-02 update with ec_alloctor.h
  2024-11-25 add class ec::blk_alloctor_g
  2024-11-9 support none ec_alloctor
  2023-5-21 update io_buffer
  2023-5-13 autobuf remove ec::memory
  2023-5-8 add ec::memory::maxblksize()
  2023-3-1 update io_buffer::append
  2022-10-10 update autobuf, mark mem_sets, block_allocator, cycle_fifo deprecated
  2022-10-8 update parsebuffer
  2022-7-24 add parsebuffer
  2022-7-20 add io_buffer

autobuf
	buffer class auto free memory

io_buffer
	for net io send

parsebuffer
	for net io read parse buffer

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <assert.h>
#include <cstdint>
#include <memory.h>
#include <vector>
#include "ec_mutex.h"

#ifndef _HAS_EC_ALLOCTOR
#define _USE_EC_OBJ_ALLOCATOR  
#endif

namespace ec {

	inline void* g_malloc(size_t size, size_t* psize = nullptr)
	{
#ifdef _HAS_EC_ALLOCTOR
		return get_ec_allocator()->malloc_(size, psize);
#else
		if (psize) {
			*psize = size;
		}
		return ::malloc(size);
#endif
	}

	inline void g_free(void* ptr)
	{
#ifdef _HAS_EC_ALLOCTOR
		return get_ec_allocator()->free_(ptr);
#else
		if (ptr) {
			return ::free(ptr);
		}
#endif
	}

	inline void* g_realloc(void* ptr, size_t size, size_t* psize = nullptr)
	{
#ifdef _HAS_EC_ALLOCTOR
		return  get_ec_allocator()->realloc_(ptr, size, psize);
#else
		void* pr = ::realloc(ptr, size);
		if (pr && psize) {
			*psize = size;
		}
		return pr;
#endif
	}

	inline void* g_calloc(size_t num, size_t size)
	{
#ifdef _HAS_EC_ALLOCTOR
		void* pr = get_ec_allocator()->malloc_(num * size);
		if (pr)
			memset(pr, 0, num * size);
		return pr;
#else
		return ::calloc(num, size);
#endif
	}

	template <typename _Tp = char>
	class autobuf
	{
	public:
#if __GNUG__ && __GNUC__ < 5
		using value_type = typename std::enable_if<__has_trivial_copy(_Tp), _Tp>::type;
#else
		using value_type = typename std::enable_if<std::is_trivially_copyable<_Tp>::value, _Tp>::type;
#endif

		autobuf(const autobuf&) = delete;
		autobuf& operator = (const autobuf&) = delete;

		autobuf() : _pbuf(nullptr), _size(0)
		{
		}
		autobuf(size_t size) : _size(size)
		{
			_pbuf = (value_type*)ec::g_malloc(size * sizeof(value_type), &_size);
			_size /= sizeof(value_type);
			if (!_pbuf)
				_size = 0;
		}
		~autobuf()
		{
			clear();
		}
		autobuf& operator = (autobuf&& v) // for move
		{
			this->~autobuf();
			_pbuf = v._pbuf;
			_size = v._size;

			v._pbuf = nullptr;
			v._size = 0;

			return *this;
		}
	private:
		value_type*   _pbuf;
		size_t  _size;
	public:
		inline value_type *data()
		{
			return _pbuf;
		}

		inline size_t size()
		{
			return _size;
		}

		void clear()
		{
			if (_pbuf) {
				ec::g_free(_pbuf);
				_pbuf = nullptr;
				_size = 0;
			}
		}

		value_type* resize(size_t rsz)
		{ // not copy old data
			clear();
			if (!rsz)
				return nullptr;
			_pbuf = (value_type*)ec::g_malloc(rsz * sizeof(value_type), &_size);
			_size /= sizeof(value_type);
			if (!_pbuf)
				_size = 0;
			return _pbuf;
		}
	};

	class blk_alloctor_g final  //全局内存块分配器，ec::tcp_c使用
	{
	private:
		size_t _sizeblk; // 内存块的大小
		size_t _numblk;
	public:
		blk_alloctor_g(size_t sizeblk, size_t numblk) : _sizeblk(sizeblk), _numblk(numblk)
		{
		}
		inline size_t sizeblk() const
		{
			return _sizeblk;
		}
		void* malloc_(size_t* poutsize)
		{
			return ec::g_malloc(_sizeblk, poutsize);
		}
		void free_(void* p)
		{
			if (p) {
				ec::g_free(p);
			}
		}
	};

#ifndef _HAS_EC_ALLOCTOR
	class null_locker final // null lock
	{
	public:
		void lock() { }
		void unlock() {}
	};

	template< class LOCKER = null_locker>
	class blk_alloctor final  // memory block alloctor
	{
	private:
		size_t _sizeblk; // 内存块的大小
	public:
		blk_alloctor(size_t sizeblk, size_t numblk) : _sizeblk(sizeblk)
		{
		}
		inline size_t sizeblk() const
		{
			return _sizeblk;
		}
		void* malloc_(size_t* poutsize)
		{
			if (poutsize) {
				*poutsize = _sizeblk;
			}
			return ::malloc(_sizeblk);
		}
		void free_(void* p)
		{
			if (p) {
				::free(p);
			}
		}
	};
#endif

	template<class BLK_ALLOCTOR = blk_alloctor<>> //BLK_ALLOCTOR default not thread safe
	class io_buffer // net IO bytes buffer,用于发送缓冲
	{
	public:
		struct blk_ {
			uint32_t pos; // read position
			uint32_t len; // append position
			blk_* pnext; //next block
			blk_() :pos(0), len(0), pnext(nullptr) {}
		};
	private:
		BLK_ALLOCTOR* _pallocator;//块分配器
		blk_* _phead;//块链头
		blk_* _ptail;//块链尾
		size_t _size;//当前字节数
		size_t _sizemax;//最大字节数

		char* pdata_(blk_* pblk_) {
			return (char*)pblk_ + sizeof(blk_);
		}

		size_t blkappend(blk_* pblk, const uint8_t* p, size_t len) // return numbytes append to pblk
		{
			if (!p || !len)
				return 0;
			size_t zadd = blksize() - pblk->len;
			if (zadd) {
				if (len < zadd)
					zadd = len;
				memcpy(pdata_(pblk) + pblk->len, p, zadd);
				pblk->len += (uint32_t)zadd;
			}
			return zadd;
		}
	public:
		io_buffer(size_t size, BLK_ALLOCTOR* pallocator) : _pallocator(pallocator)
			, _phead(nullptr), _ptail(nullptr), _size(0), _sizemax(size) {
		}

		io_buffer(io_buffer&& v) noexcept //move construct
		{
			_pallocator = v._pallocator;
			_phead = v._phead;
			_ptail = v._ptail;
			_size = v._size;
			_sizemax = v._sizemax;

			v._pallocator = nullptr;
			v._phead = nullptr;
			v._ptail = nullptr;
			v._size = 0;
			v._sizemax = 0;
		}

		~io_buffer() {
			clear();
			_sizemax = 0;
		}

		io_buffer& operator = (io_buffer&& v) noexcept // for move
		{
			this->~io_buffer();
			_pallocator = v._pallocator;
			_phead = v._phead;
			_ptail = v._ptail;
			_size = v._size;
			_sizemax = v._sizemax;

			v._pallocator = nullptr;
			v._phead = nullptr;
			v._ptail = nullptr;
			v._size = 0;
			v._sizemax = 0;
			return *this;
		}

		void clear() {
			blk_* pnext;
			while (_phead) {
				pnext = _phead->pnext;
				_pallocator->free_(_phead);
				_phead = pnext;
			}
			_phead = nullptr;
			_ptail = nullptr;
			_size = 0;
		}

		inline bool empty() {
			return (_phead == nullptr || 0 == _phead->len);
		}

		inline size_t blksize() {
			return _pallocator->sizeblk() - sizeof(blk_);
		}

		inline bool blkfull(blk_* pblk) {
			return blksize() == pblk->len;
		}

		inline bool oversize() {
			return _size > _sizemax;
		}

		inline size_t size() {
			return _size;
		}
		inline size_t sizemax() {
			return _sizemax;
		}
		inline void setsizemax(size_t sizemax) {
			_sizemax = sizemax;
		}
		int waterlevel() //水位,百分数.
		{
			size_t numblk = _size * 10000;
			return static_cast<int>(numblk / _sizemax);
		}

		/**
		 * @brief append to buffer
		 * @param pdata  data
		 * @param len  bytes of pdata
		 * @param pzappendsize out append size
		 * @return if append size equal len return true, else return false;
		*/
		bool append(const void* pdata, size_t len, size_t* pzappendsize = nullptr)
		{
			if (pzappendsize)
				*pzappendsize = 0;
			if (!pdata || !len)
				return true;
			const uint8_t* p = (uint8_t*)pdata;
			size_t zadd = 0, zt;
			while (zadd < len) {
				if (!_ptail || blkfull(_ptail)) {
					if (oversize())
						return false;
					blk_* p = (blk_*)_pallocator->malloc_(nullptr);
					if (!p)
						return false;
					new(p)blk_();
					if (!_phead) {
						_ptail = p;
						_phead = _ptail;
					}
					else {
						_ptail->pnext = p;
						_ptail = p;
					}
				}
				zt = blkappend(_ptail, p + zadd, len - zadd);
				_size += zt;
				zadd += zt;
				if (pzappendsize)
					*pzappendsize += zt;
			}
			return true;
		}

		//从头部获取数据块,返回数据库指针,zlen回填长度。无拷贝
		const void* get(size_t& zlen)
		{
			blk_* pnext;
			const uint8_t* pret = nullptr;
			zlen = 0;
			while (_phead) {
				if (_phead->pos == _phead->len) {
					pnext = _phead->pnext;
					_pallocator->free_(_phead);
					_phead = pnext;
					if (!_phead) {
						_ptail = nullptr;
						return nullptr;
					}
					continue;
				}
				pret = (const uint8_t*)pdata_(_phead) + _phead->pos;
				zlen = _phead->len - _phead->pos;
				break;
			}
			return pret;
		}

		//从头部开始释放zlen长度数据,当调用get后使用。
		void freesize(size_t zlen)
		{
			blk_* pnext;
			size_t zfree = 0, zt;
			while (zfree < zlen && _phead) {
				zt = zlen - zfree;
				if (_phead->len - _phead->pos > zt) {
					_phead->pos += (uint32_t)zt;
					zfree += zt;
				}
				else {
					zfree += _phead->len - _phead->pos;
					pnext = _phead->pnext;
					_pallocator->free_(_phead);
					_phead = pnext;
					if (!_phead)
						_ptail = nullptr;
				}
			}
			_size = _size >= zfree? _size - zfree : 0;
		}
	};//io_buffer

	class parsebuffer //协议解析优化缓冲,去掉解析时移动拷贝
	{
	public:
		parsebuffer() : _head(0), _tail(0), _bufsize(0), _pos(0), _pbuf(nullptr) {
		}
		~parsebuffer() {
			free();
		}

		parsebuffer(parsebuffer&& v) noexcept //move construct
		{
			_head = v._head;
			_tail = v._tail;
			_bufsize = v._bufsize;
			_pos = v._pos;
			_pbuf = v._pbuf;

			v._head = 0;
			v._tail = 0;
			v._bufsize = 0;
			v._pos = 0;
			v._pbuf = nullptr;
		}

		parsebuffer& operator = (parsebuffer&& v) noexcept // for move
		{
			this->~parsebuffer();

			_head = v._head;
			_tail = v._tail;
			_bufsize = v._bufsize;
			_pbuf = v._pbuf;
			_pos = v._pos;

			v._head = 0;
			v._tail = 0;
			v._bufsize = 0;
			v._pos = 0;
			v._pbuf = nullptr;

			return *this;
		}

	private:
		size_t _head;// next read position >=0
		size_t _tail;// next write position <= _bufsize
		size_t _bufsize;
		size_t _pos;  // read write as stream, position from _head
		uint8_t* _pbuf;
	private:
		void* malloc_(size_t size, size_t& outsize) {
			if (size < 1000 * 8) {
				size = 1000 * 8;
			}
			return ec::g_malloc(size, &outsize);
		}
		inline void free_(void* p) {
			ec::g_free(p);
		}
	public:
		inline size_t size_() const //数据长度
		{
			return _tail - _head;
		}
		inline size_t bufsize() const {
			return _bufsize;
		}
		inline bool empty() const{
			return !_pbuf || (_tail == _head);
		}

		int append(const void* pdata, size_t size) // return 0:ok; -1:error
		{
			if (!pdata || !size)
				return 0;
			if (!_pbuf) {
				_pbuf = (uint8_t*)malloc_(size + size / 2, _bufsize);
				if (_pbuf) {
					memcpy(_pbuf, pdata, size);
					_pos = 0;
					_head = 0;
					_tail = size;
					return 0;
				}
				return -1;
			}
			if (_tail + size <= _bufsize) {
				memcpy(_pbuf + _tail, pdata, size);
				_tail += size;
				return 0;
			}

			//空间不够,重新分配1.5倍空间
			size_t oldsize = _tail - _head;
			size_t newbufsize = oldsize + size;
			newbufsize += newbufsize / 2;
			uint8_t* pnew = (uint8_t*)malloc_(newbufsize, newbufsize);
			if (!pnew)
				return -1;
			memcpy(pnew, _pbuf + _head, oldsize);
			memcpy(pnew + oldsize, pdata, size);
			_head = 0;
			_tail = oldsize + size;
			_bufsize = newbufsize;
			free_(_pbuf);
			_pbuf = pnew;
			return 0;
		}

		void* data_() //数据头部
		{
			return _pbuf + _head;
		}

		void freehead(size_t size) //从头释放size字节
		{
			if (!_pbuf)
				return;
			_head += size;
			if (_head >= _tail) {
				_head = 0;
				_tail = 0;
				_pos = 0;
				if (_bufsize > 1024 * 32) {
					_bufsize = 0;
					free_(_pbuf);
					_pbuf = nullptr;
				}
			}
		}

		void free()//全部释放并释放缓冲区
		{
			if (_pbuf) {
				_head = 0;
				_tail = 0;
				_bufsize = 0;
				_pos = 0;
				free_(_pbuf);
				_pbuf = nullptr;
			}
		}

		static bool is_be() // is big endian
		{
			union {
				uint32_t u32;
				uint8_t u8;
			} ua;
			ua.u32 = 0x01020304;
			return ua.u8 == 0x01;
		}

		parsebuffer& setpos(size_t pos) noexcept
		{
			_pos = _head + pos > _tail ? _tail - _head : pos;
			return *this;
		}

		inline void posend() noexcept
		{
			_pos = _tail - _head;
		}

		inline void posbegin() noexcept
		{
			_pos = 0;
		}

		inline size_t getpos() const noexcept
		{
			return _pos;
		}

		parsebuffer& read(void* pdata, size_t size)
		{ // read block from current positiion
			if (_pos + _head + size > _tail)
				throw std::range_error("oversize");
			memcpy(pdata, _pbuf + _head + _pos, size);
			_pos += size;
			return *this;
		}

		parsebuffer& write(const void* pdata, size_t size)
		{ // write block to current positiion
			if (_head + _pos + size <= _tail) {
				memcpy(_pbuf + _head + _pos, pdata, size);
			}
			else {
				size_t zcp = _tail - _head - _pos;
				if (zcp > 0)
					memcpy(_pbuf + _head + _pos, pdata, zcp);
				append((const char*)pdata + zcp, size - zcp);
			}
			_pos += size;
			return *this;
		}

		template < typename T
			, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		parsebuffer& operator >> (T& v)
		{ // read as little_endian from current positiion
			if (_head + _pos + sizeof(T) > _tail)
				throw std::range_error("oversize");
			if (!is_be())
				memcpy(&v, _pbuf + _head + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* ps = _pbuf + _head + _pos, * pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T
			, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		parsebuffer& operator > (T& v)
		{ // read as big_endian from current positiion
			if (_head + _pos + sizeof(T) > _tail)
				throw std::range_error("oversize");
			if (is_be())
				memcpy(&v, _pbuf + _head + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* ps = _pbuf + _head + _pos, * pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T
			, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		parsebuffer& operator << (T v)
		{ // write as little_endian to current positiion
			if (_head + _pos + sizeof(T) > _tail) {
				char uap[sizeof(T)] = { 0 };
				append(uap, _head + _pos + sizeof(T) - _tail); //grwon fill zero
			}
			if (!is_be())
				memcpy(_pbuf + _head + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* pd = _pbuf + _head + _pos, * ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T
			, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		parsebuffer& operator < (T v)
		{ // write as big_endian to current positiion
			if (_head + _pos + sizeof(T) > _tail) {
				char uap[sizeof(T)] = { 0 };
				append(uap, _head + _pos + sizeof(T) - _tail);//grwon fill zero
			}
			if (is_be())
				memcpy(_pbuf + _head + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* pd = _pbuf + _head + _pos, * ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		uint8_t operator[] (size_t n) noexcept
		{
			return _pbuf[_head + n];
		}

		const uint8_t operator[] (size_t n) const noexcept
		{
			return _pbuf[_head + n];
		}
	};
}
