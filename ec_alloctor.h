/*!
\file ec_alloctor.h
\author	jiangyong
\email  kipway@outlook.com
\update
  2024.12.02 update memory block initialization
  2024.11.29 remove clib malloc
  2024.11.07 add define _HAS_EC_ALLOCTOR

memory
	eclib memory allocator for ec::string, ec::hashmap, and other small objects etc.

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <cassert>
#include <atomic>
#include <string.h>

#define _ZLIB_SELF_ALLOC
#define _HAS_EC_ALLOCTOR

#include "ec_mutex.h"
#ifndef EC_ALLOCTOR_ALIGN
#define EC_ALLOCTOR_ALIGN 8u
#endif

#ifndef EC_ALLOCTOR_SHEAP_SIZE  // small heap size
#if defined(_MEM_TINY) // < 256M
#define EC_ALLOCTOR_SHEAP_SIZE (64 * 1024) // 80K heap size
#elif defined(_MEM_SML) // < 1G
#define EC_ALLOCTOR_SHEAP_SIZE (256 * 1024) // 256K heap size
#else
#define EC_ALLOCTOR_SHEAP_SIZE (1024 * 1024) // 1M heap size
#endif
#endif

#ifndef EC_ALLOCTOR_MHEAP_SIZE // middle heap size
#if defined(_MEM_TINY) // < 256M
#define EC_ALLOCTOR_MHEAP_SIZE (128 * 1024) // 128K heap size
#elif defined(_MEM_SML) // < 1G
#define EC_ALLOCTOR_MHEAP_SIZE (512 * 1024) // 512K heap size
#else
#define EC_ALLOCTOR_MHEAP_SIZE (2 * 1024 * 1024) // 1M heap size
#endif
#endif

#ifndef EC_ALLOCTOR_HHEAP_SIZE // huge heap size
#if defined(_MEM_TINY) // < 256M
#define EC_ALLOCTOR_HHEAP_SIZE (256 * 1024) // 256K heap size
#elif defined(_MEM_SML) // < 1G
#define EC_ALLOCTOR_HHEAP_SIZE (1 * 1024 * 1024) // 1M heap size
#else
#define EC_ALLOCTOR_HHEAP_SIZE (4 * 1024 * 1024) // 4M heap size
#endif
#endif

#ifndef EC_ALLOCTOR_GC_MINHEAPS // start garbage collection min number of heaps
#if defined(_MEM_TINY) // < 256M
#define EC_ALLOCTOR_GC_MINHEAPS 1
#elif defined(_MEM_SML) // < 1G
#define EC_ALLOCTOR_GC_MINHEAPS 2
#else
#define EC_ALLOCTOR_GC_MINHEAPS 3
#endif
#endif

#ifndef EC_SIZE_BLK_ALLOCATOR
#define EC_SIZE_BLK_ALLOCATOR 40
#endif

#define EC_ALLOCTOR_LARGEMEM_HEADSIZE (2 * EC_ALLOCTOR_ALIGN) // head size of large memory

#ifdef _WIN32
#include <memoryapi.h>
namespace ec {
	struct glibc_noarena{
	};
	inline void* blk_sysmalloc(size_t size, size_t* psize) {
		void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (ptr && psize) {
			MEMORY_BASIC_INFORMATION mbiStatus;
			memset(&mbiStatus, 0, sizeof(mbiStatus));
			if (VirtualQuery(ptr, &mbiStatus, sizeof(mbiStatus))) {
				*psize = mbiStatus.RegionSize;
			}
			else {
				*psize = size;
			}
		}
		return ptr;
	}
	inline bool blk_sysfree(void* ptr, size_t size) { // windows size not be used
		return VirtualFree(ptr, 0, MEM_RELEASE);
	}
}
#else
#include <sys/mman.h>
#include <malloc.h>
namespace ec {
	struct glibc_noarena {
		glibc_noarena() {
			mallopt(M_ARENA_MAX, 1);
			mallopt(M_MMAP_THRESHOLD, EC_ALLOCTOR_SHEAP_SIZE);
			mallopt(M_TRIM_THRESHOLD, EC_ALLOCTOR_SHEAP_SIZE);
		}
	};
	inline void* blk_sysmalloc(size_t size, size_t* psize) {
		size_t zp = sysconf(_SC_PAGE_SIZE);
		if (size % zp) {
			size += zp - (size % zp);
		}
		if (psize) {
			*psize = size;
		}
		return mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	}
	inline bool blk_sysfree(void* ptr, size_t size) {
		return !munmap(ptr, size);
	}
}
#endif
#define DECLARE_EC_GLIBC_NOARENA ec::glibc_noarena g_ec_glibc_noarena;

namespace ec {
	class null_lock final { // null lock for lockfree
	public:
		void lock() { }
		void unlock() {}
	};
	class objsets_ final
	{
	public:
		struct t_blk {
			struct t_blk* pnext;
		};
		size_t _sizearea{ 0 };// size blk_sysmalloc 
		size_t _sizeobj{ 0 }; // size of object
		size_t _numfree{ 0 }; // number of free blocks
		size_t _numobjs{ 0 }; // number of capacity
		size_t _posnext{ 0 }; // position for next alloc
		t_blk* _phead{ nullptr }; // head block
		objsets_* _pnext{ nullptr }; // next objsets
		char _mem[];
	public:
		objsets_(size_t sizearea, size_t sizeobj) : _sizearea(sizearea) {
			_sizeobj = sizeobj;
			if (_sizeobj % 16) {
				_sizeobj += 16 - (_sizeobj % 16);
			}
			_numobjs = (sizearea - sizeof(objsets_)) / _sizeobj;
			_numfree = _numobjs;
			_pnext = nullptr;
		}
		inline bool empty() const {
			return 0 == _numfree;
		}
		inline bool canfree() const {
			return _numobjs == _numfree;
		}
		void* malloc_()
		{
			if (!_phead) {
				if (_posnext < _numobjs) {
					char* pret = _mem + _posnext * _sizeobj;
					++_posnext;
					--_numfree;
					return pret;
				}
				return nullptr;
			}
			t_blk* pret = _phead;
			_phead = _phead->pnext;
			--_numfree;
			return pret;
		}
		bool free_(void* pf) {
			if (!pf)
				return false;
			char* p = (char*)pf;
			if (p >= _mem && p < _mem + _sizeobj * _numobjs) {
				t_blk* pblk = (t_blk*)pf;
				pblk->pnext = _phead;
				_phead = pblk;
				++_numfree;
				return true;
			}
			return false;
		}
	};

	template <class LOCK = null_lock>
	class objalloctor_ final // object allcotro for memheap_, blk_alloctor etc.
	{
	protected:
		size_t _sizearea; // size blk_sysmalloc
		size_t _sizeobj;
		size_t _numfreeblks{ 0 };
		objsets_* _phead{ nullptr };
		LOCK _lck;
		objsets_* newobj() {
			size_t zlen = 0;
			void* ptr = ec::blk_sysmalloc(_sizearea, &zlen);
			if (!ptr)
				return nullptr;
			return new(ptr) objsets_(zlen, _sizeobj);
		}
		inline bool deleteobj(objsets_* pobj) {
			return ec::blk_sysfree(pobj, pobj->_sizearea);
		}
	public:
		objalloctor_(size_t sizearea, size_t sizeobj) :_sizearea(sizearea), _sizeobj(sizeobj) {
			_phead = newobj();
			if (_phead) {
				_numfreeblks += _phead->_numobjs;
			}
		}
		~objalloctor_() {
			objsets_* pprior = nullptr;
			while (_phead) {
				pprior = _phead;
				_phead = _phead->_pnext;
				deleteobj(pprior);
			}
			_numfreeblks = 0;
		}
		void* malloc_(size_t sizeobj) {
			safe_lock<LOCK> lck(&_lck);
			if (sizeobj > _sizeobj)
				return nullptr;
			objsets_* pheap;
			if (!_numfreeblks) {
				pheap = newobj();
				if (!pheap)
					return nullptr;
				pheap->_pnext = _phead;
				_phead = pheap;
				_numfreeblks += _phead->_numobjs;
			}
			void* pret = nullptr;
			objsets_* pprior = nullptr;
			pheap = _phead;
			while (pheap) {
				if (nullptr != (pret = pheap->malloc_())) {
					--_numfreeblks;
					if (pprior && pheap->_numfree > 0) { // move to head for next fast malloc
						pprior->_pnext = pheap->_pnext;
						pheap->_pnext = _phead;
						_phead = pheap;
					}
					return pret;
				}
				pprior = pheap;
				pheap = pheap->_pnext;
			}
			return nullptr;
		}
		bool free_(void* ptr) {
			safe_lock<LOCK> lck(&_lck);
			objsets_* pprior = nullptr;
			objsets_* pheap = _phead;
			while (pheap) {
				if (pheap->free_(ptr)) {
					++_numfreeblks;
					if (pprior && pheap->canfree()) {
						pprior->_pnext = pheap->_pnext;
						_numfreeblks -= pheap->_numobjs;
						return deleteobj(pheap);
					}
					return true;
				}
				pprior = pheap;
				pheap = pheap->_pnext;
			}
			return false;
		}
		int prtinfo() {
			safe_lock<LOCK> lck(&_lck);
			int nheaps = 0;
			objsets_* pheap = _phead;
			printf("  heap objects:\n");
			while (pheap) {
				++nheaps;
				printf("  numobjs =  %6zu, freeobjs = %6zu, sizearea = %8zu, sizeobj = %6zu\n",
					pheap->_numobjs, pheap->_numfree, pheap->_sizearea, pheap->_sizeobj);
				pheap = pheap->_pnext;
			}
			printf("  numheaps = %d, numfreeblks = %zu\n", nheaps, _numfreeblks);
			return 0;
		}
		template<class STR_ = std::string>
		void meminfo(STR_& sout) { // for debug
			safe_lock<LOCK> lck(&_lck);
			int nheaps = 0, nl;
			objsets_* pheap = _phead;
			char stmp[400];
			sout.append("  heap objects:");
			while (pheap) {
				sout.push_back('\n');
				++nheaps;
				nl = snprintf(stmp, sizeof(stmp), "  numobjs =  %6zu, freeobjs = %6zu, sizearea = %8zu, sizeobj = %6zu",
					pheap->_numobjs, pheap->_numfree, pheap->_sizearea, pheap->_sizeobj);
				if (nl < (int)sizeof(stmp)) {
					sout.append(stmp, nl);
				}
				pheap = pheap->_pnext;
			}
			nl = snprintf(stmp, sizeof(stmp), "\n  numheaps = %d, numfreeblks = %zu \n", nheaps, _numfreeblks);
			sout.append(stmp, nl);
		}
	};
}// namespace ec

namespace ec{
	class memheap_ final // memory heap, memory blocks of the same size
	{
		struct t_blk {
			struct t_blk* pnext;
		};
	protected:
		uint32_t _numfree;// number of free blocks
		uint32_t _numblk;//number of all blocks
		uint32_t _sizeblk;// sizeof block
		uint32_t _posnext;// position for next alloc
		size_t _sizesystem; // size actually allocated from the system
		char* _pmem;// memory allocated from the system
		t_blk* _phead; // head block pointer of list
		void* _palloc; //blk_alloctor for release optimization
	public:
		memheap_* _pnext; //point to next heap
		inline size_t numfree() const {
			return _numfree;
		}
		inline size_t numblk() const {
			return _numblk;
		}
		inline size_t sizeblk() const {
			return _sizeblk;
		}
		inline void* getalloc() const {
			return _palloc;
		}
		void* ptrapp() const {
			if (_pmem) {
				return _pmem + EC_ALLOCTOR_ALIGN;
			}
			return nullptr;
		}
		inline bool single_blk() const {
			return (!_palloc && !_numblk);
		}
		static void* operator new(size_t size);
		static void operator delete(void* p);
		static void* operator new(size_t size, void* ptr) { return ptr; }
		static void operator delete(void* ptr, void* voidptr2) noexcept {}
	public:
		memheap_(void* palloc) :_numfree(0), _numblk(0), _sizeblk(0), _posnext(0), _sizesystem(0), _pmem(nullptr),
			_phead(nullptr), _palloc(palloc), _pnext(nullptr) {
		}
		~memheap_() {
			if (_pmem) {
				ec::blk_sysfree(_pmem, _sizesystem);
				_numfree = 0;
				_numblk = 0;
				_posnext = 0;
				_sizeblk = 0;
				_pmem = nullptr;
				_sizesystem = 0;
				_phead = nullptr;
				_palloc = nullptr;
				_pnext = nullptr;
			}
		}
		bool init(size_t sizeblk, size_t numblk) {
			if (sizeblk % EC_ALLOCTOR_ALIGN)
				sizeblk += (EC_ALLOCTOR_ALIGN - sizeblk % EC_ALLOCTOR_ALIGN);
			_pmem = (char*)ec::blk_sysmalloc(numblk * (sizeblk + EC_ALLOCTOR_ALIGN), &_sizesystem);
			if (!_pmem)
				return false;
			_sizeblk = (uint32_t)sizeblk;
			_numblk = (uint32_t)numblk;
			_numfree = _numblk;
			return true;
		}
		void* malloc_() {
			if (!_phead) {
				if (_posnext < _numblk) {
					char* ps = (_pmem + _posnext * (size_t)(_sizeblk + EC_ALLOCTOR_ALIGN));
					*reinterpret_cast<memheap_**>(ps) = this;
					++_posnext;
					--_numfree;
					return ps + EC_ALLOCTOR_ALIGN;
				}
				return nullptr;
			}
			t_blk* pret = _phead;
			_phead = _phead->pnext;
			--_numfree;
			return pret;
		}
		bool free_(void* pf) {
			t_blk* pblk = (t_blk*)pf;
			pblk->pnext = _phead;
			_phead = pblk;
			++_numfree;
			return true;
		}
		bool empty() const {
			return 0u == _numfree;
		}
		bool canfree() const {
			return _numblk == _numfree;
		}
	}; //memblk_

	template<class LOCK = null_lock>
	class blk_alloctor final  // memory block alloctor
	{
	protected:
		memheap_* _phead;
		int32_t _numheaps; // 堆个数，用于辅助释放空闲堆
		int32_t _numfreeblks; // 空闲内存块数,用于快速增加堆
		uint32_t _sizeblk; // 内存块的大小，构造或者初始化时设定,EC_ALLOCTOR_ALIGN字节对齐.
		uint32_t _numblksperheap; //每个堆里的内存块个数
		LOCK _lck;
	public:
		inline int32_t numheaps() const	{
			return _numheaps;
		}
		inline size_t sizeblk() const {
			return _sizeblk;
		}
		inline int numfreeblks() const {
			return _numfreeblks;
		}
		inline uint32_t numBlksPerHeap() {
			return _numblksperheap;
		}
		static void* operator new(size_t size);
		static void operator delete(void* p);
		static void* operator new(size_t size, void* ptr) { return ptr; }
		static void operator delete(void* ptr, void* voidptr2) noexcept {}
		blk_alloctor() :
			_phead(nullptr),
			_numheaps(0),
			_numfreeblks(0),
			_sizeblk(0),
			_numblksperheap(0) {
		}
		blk_alloctor(size_t sizeblk, size_t numblk) :
			_phead(nullptr),
			_numheaps(0),
			_numfreeblks(0),
			_sizeblk(0),
			_numblksperheap(0) {
			init(sizeblk, numblk);
		}
		~blk_alloctor() {
			memheap_* p = _phead, * pn;
			while (p) {
				pn = p->_pnext;
				delete p;
				p = pn;
			}
			_phead = nullptr;
			_numheaps = 0;
			_numfreeblks = 0;
			_sizeblk = 0;
			_numblksperheap = 0;
		}
		bool init(size_t sizeblk, size_t numblk, bool balloc = true) {
			if (_phead) {
				return true;
			}
			if (!balloc) {
				if (sizeblk % EC_ALLOCTOR_ALIGN)
					sizeblk += (EC_ALLOCTOR_ALIGN - sizeblk % EC_ALLOCTOR_ALIGN);
				_sizeblk = (uint32_t)sizeblk;
				_numblksperheap = (uint32_t)numblk;
				return true;
			}
			_phead = new memheap_(this);
			if (!_phead) {
				return false;
			}
			if (!_phead->init(sizeblk, numblk)) {
				delete _phead;
				_phead = nullptr;
				return false;
			}
			_sizeblk = (uint32_t)_phead->sizeblk();
			_numblksperheap = (uint32_t)numblk;
			_numheaps = 1;
			_numfreeblks += (int)_numblksperheap;
			return true;
		}
		void* malloc_(size_t* poutsize) {
			safe_lock<LOCK> lck(&_lck);
			memheap_* pheap;
			if (!_numfreeblks) {
				pheap = new memheap_(this);
				if (!pheap)
					return nullptr;
				if (!pheap->init(_sizeblk, _numblksperheap)) {
					delete pheap;
					return nullptr;
				}
				pheap->_pnext = _phead;
				_phead = pheap;
				_numheaps++;
				_numfreeblks += (int32_t)(_numblksperheap - 1);
				if (poutsize) {
					*poutsize = _sizeblk;
				}
				return _phead->malloc_();
			}
			void* pret = nullptr;
			memheap_* pprior = nullptr;
			pheap = _phead;
			while (pheap) {
				if (nullptr != (pret = pheap->malloc_())) {
					_numfreeblks--;
					if (poutsize) {
						*poutsize = _sizeblk;
					}
					if (pprior && pheap->numfree() > 0) { // move to head for next fast malloc
						pprior->_pnext = pheap->_pnext;
						pheap->_pnext = _phead;
						_phead = pheap;
					}
					return pret;
				}
				pprior = pheap;
				pheap = pheap->_pnext;
			}
			assert(pret != nullptr);
			return nullptr;
		}
		bool free_(void* p) {// for single allotor such as ec::hashmap
			memheap_** pheap = (memheap_**)(static_cast<char*>(p) - EC_ALLOCTOR_ALIGN);
			assert(*pheap);
			_lck.lock();
			assert((*pheap)->getalloc() == this);
			(*pheap)->free_(p);
			_numfreeblks++;
			if (_numheaps > EC_ALLOCTOR_GC_MINHEAPS && (*pheap)->canfree() && (*pheap) != _phead
				&& _numfreeblks != (int32_t)_numblksperheap) {
				gc_();
			}
			_lck.unlock();
			return true;
		}
		bool free_(memheap_* pheap, void* p) { // for multiple allotor
			_lck.lock();
			pheap->free_(p);
			_numfreeblks++;
			if (_numheaps > EC_ALLOCTOR_GC_MINHEAPS && pheap->canfree() && pheap != _phead
				&& _numfreeblks != (int32_t)_numblksperheap) {
				gc_();
			}
			_lck.unlock();
			return true;
		}
		size_t numfree() {// for debug
			safe_lock<LOCK> lck(&_lck);
			size_t zr = 0u;
			memheap_* p = _phead;
			while (p) {
				zr += p->numfree();
				p = p->_pnext;
			}
			return zr;
		}
	private:
		int gc_() { //garbage collection
			int n = 0;
			memheap_* p = _phead; //_phead永远不会被回收
			memheap_* pnext;
			if (!p) {
				return n;
			}
			pnext = p->_pnext;
			while (pnext) {
				if (pnext->canfree()) {
					p->_pnext = pnext->_pnext;
					delete pnext;
					_numheaps--;
					_numfreeblks -= (int)(_numblksperheap);
					n++;
				}
				else {
					p = pnext;
				}
				pnext = p->_pnext;
			}
			return n;
		}
	};

	class allocator final
	{
	protected:
		using PA_ = blk_alloctor<spinlock>*;
		unsigned int _size;
		std::atomic_int _nLargeMems{ 0 };// Number of remaining large memory blocks, used for memory leak detection
		PA_ _alloctors[EC_SIZE_BLK_ALLOCATOR];
	private:
		void* malloc__(size_t size, size_t* psize = nullptr) { // size <= maxblksize()
			void* pret = nullptr;
			uint32_t i = 0u;
			if (_size > 16 && size > 1000) { // 1/2 find
				int nl = 0, nh = (int)_size - 1, nm = 0;
				while (nl <= nh) {
					nm = (nl + nh) / 2;
					if (_alloctors[nm]->sizeblk() < size)
						nl = nm + 1;
					else if (_alloctors[nm]->sizeblk() > size)
						nh = nm - 1;
					else
						break;
				}
				i = nm;
			}
			for (; i < _size; i++) {
				if (_alloctors[i]->sizeblk() >= size) {
					pret = _alloctors[i]->malloc_(psize);
					return pret;
				}
			}
			return pret;
		}

		void* largeMalloc(size_t size, size_t* psize = nullptr) {
			size += (EC_ALLOCTOR_ALIGN * 2);
			size_t zlen = 0;
			char* ptr = (char*)blk_sysmalloc(size, &zlen);
			if (ptr) {
				memset(ptr, 0, EC_ALLOCTOR_LARGEMEM_HEADSIZE);
				*((size_t*)ptr) = zlen;
				if (psize) {
					*psize = zlen - EC_ALLOCTOR_LARGEMEM_HEADSIZE;
				}
				++_nLargeMems;
			}
			return ptr + EC_ALLOCTOR_LARGEMEM_HEADSIZE;
		}

		bool largeFree(void* ptr) {
			if (!ptr)
				return false;
			size_t* pmem = (size_t*)(reinterpret_cast<char*>(ptr) - EC_ALLOCTOR_LARGEMEM_HEADSIZE);
			--_nLargeMems;
			return blk_sysfree(pmem, *pmem);
		}
	public:
		bool add_alloctor(size_t sizeblk, size_t numblk, bool balloc = true) {
			if (_size == sizeof(_alloctors) / sizeof(PA_))
				return false;
			PA_ p = new blk_alloctor<spinlock>;
			if (!p->init(sizeblk, numblk, balloc)) {
				delete p;
				return false;
			}
			_alloctors[_size++] = p;
			return true;
		}
	public:
		allocator() :_size(0), _alloctors{ nullptr } {
		}
		~allocator() {
			for (auto i = 0u; i < _size; i++) {
				if (_alloctors[i]) {
					delete _alloctors[i];
					_alloctors[i] = nullptr;
				}
			}
			_size = 0;
		}
		size_t maxblksize() const {
			return 0u == _size ? 0 : _alloctors[_size - 1]->sizeblk();
		}
		void* malloc_(size_t size, size_t* psize = nullptr)	{
			if (size > maxblksize()) {
				return largeMalloc(size, psize);
			}
			return malloc__(size, psize);
		}
		void* realloc_(void* ptr, size_t size, size_t* poutsize = nullptr) {
			if (!ptr) { // malloc
				if (!size)
					return nullptr;
				return malloc_(size, poutsize);
			}
			if (!size) { // free
				free_(ptr);
				return nullptr;
			}
			size_t sizeorg;
			memheap_** pheap = (memheap_**)(reinterpret_cast<char*>(ptr) - EC_ALLOCTOR_ALIGN);
			if (!*pheap) {
				sizeorg = *((size_t*)(reinterpret_cast<char*>(ptr) - EC_ALLOCTOR_LARGEMEM_HEADSIZE)) - EC_ALLOCTOR_LARGEMEM_HEADSIZE;
			}
			else {
				sizeorg = (*pheap)->sizeblk();
			}
			if (sizeorg >= size)
				return ptr;
			char* pnew = (char*)malloc_(size, poutsize);
			if (!pnew)
				return nullptr;
			memcpy(pnew, ptr, sizeorg);
			free_(ptr);
			return pnew;
		}
		void free_(void* p) {
			if (!p)
				return;
			memheap_** pheap = (memheap_**)(reinterpret_cast<char*>(p) - EC_ALLOCTOR_ALIGN);
			if (!*pheap) {
				largeFree(p);
			}
			else {
				reinterpret_cast<blk_alloctor<spinlock>*>((*pheap)->getalloc())->free_(*pheap, p);
			}
		}
		int prtinfo() { // for debug
			printf("\nprintf ec::alloctor{\n");
			printf("  numLargeMemorys = %d\n", static_cast<int>(_nLargeMems));
			for (auto i = 0u; i < _size; i++) {
				printf("  blockSize = %8zu, blockPerHeap = %8u, numHeaps = %4d, FreeBlocks = %8d, calculate FreeBlocks=%zu\n",
					_alloctors[i]->sizeblk(), _alloctors[i]->numBlksPerHeap(), _alloctors[i]->numheaps(),
					_alloctors[i]->numfreeblks(), _alloctors[i]->numfree());
			}
			printf("}\n\n");
			return 0;
		}
		template<class STR_ = std::string>
		void meminfo(STR_& sout, int ndebug = 0) { // for debug
			char stmp[400];
			sout.append("\n");
			int nl = snprintf(stmp, sizeof(stmp), "  Large memory:\n  numLargeMemorys = %d\n  heap info:", static_cast<int>(_nLargeMems));
			sout.append(stmp, nl);
			for (auto i = 0u; i < _size; i++) {
				sout.push_back('\n');
				nl = snprintf(stmp, sizeof(stmp), "  blockSize = %8zu, blockPerHeap = %8u, numHeaps = %4d, FreeBlocks = %8d",
					_alloctors[i]->sizeblk()
					, _alloctors[i]->numBlksPerHeap()
					, _alloctors[i]->numheaps()
					, _alloctors[i]->numfreeblks()
				);
				if(nl < (int)sizeof(stmp))
					sout.append(stmp, nl);
				if (ndebug) {
					sprintf(stmp, ", calculate = %8zu\n", _alloctors[i]->numfree());
					sout.append(stmp);
				}
			}
			sout.append("\n");
		}
	};
	constexpr size_t zbaseobjsize = sizeof(memheap_) > sizeof(blk_alloctor<spinlock>) ? sizeof(memheap_) : sizeof(blk_alloctor<spinlock>);
	constexpr size_t zselfblksize = (zbaseobjsize % 8u) ? zbaseobjsize + 8u - zbaseobjsize % 8u : zbaseobjsize;
	using selfalloctor = objalloctor_<spinlock>;
#ifndef SELFOBJ_AREA_SIZE
#if defined(_MEM_TINY)
	#define SELFOBJ_AREA_SIZE (32 * 1024)
#elif defined(_MEM_SML)
	#define SELFOBJ_AREA_SIZE (128 * 1024)
#else
	#define SELFOBJ_AREA_SIZE (512 * 1024)
#endif
#endif
}//ec

ec::selfalloctor* get_ec_alloctor_self(); //self use global memory allocator for memblk_, blk_alloctor

#define DECLARE_EC_ALLOCTOR ec_allocator_ g_ec_allocator;\
ec::selfalloctor* get_ec_alloctor_self() { return &g_ec_allocator._alloctor_self; }\
ec::allocator* get_ec_allocator() { return &g_ec_allocator._alloctor; }\
void* ec::memheap_::operator new(size_t size){\
	return get_ec_alloctor_self()->malloc_(size);\
}\
void ec::memheap_::operator delete(void* p){\
	get_ec_alloctor_self()->free_(p);\
}\
template<class LOCK>\
 void* ec::blk_alloctor<LOCK>::operator new(size_t size){\
	 return get_ec_alloctor_self()->malloc_(size);\
 }\
 template<class LOCK>\
 void ec::blk_alloctor<LOCK>::operator delete(void* p) {\
	 get_ec_alloctor_self()->free_(p);\
 }

ec::allocator* get_ec_allocator();
class ec_allocator_ {
public:
	ec_allocator_(): _alloctor_self(SELFOBJ_AREA_SIZE, ec::zselfblksize) {
#if defined(_MEM_TINY) || defined(_MEM_SML)
		size_t sheaps[8]{ 16, 32, 64, 128, 256, 512, 1024, 2 * 1024 };
		size_t mheaps[4]{ 4 * 1024, 8 * 1024, 16 * 1024, 32 * 1024 };
		size_t lheaps[2]{ 64 * 1024, 128 * 1024 };
		size_t hheaps[3]{ 256 * 1024, 512 * 1024, 1024 * 1024 };
#else
		size_t sheaps[12]{ 16, 32, 64, 96, 128, 256, 384, 512, 640, 800, 1024, 1424 };
		size_t mheaps[11]{ 2 * 1024, 3 * 1024, 5 * 1024, 8 * 1024, 12 * 1024, 16 * 1024, 20 * 1024, 24 * 1024, 32 * 1024, 48 * 1024, 64 * 1024, };
		size_t lheaps[4]{ 128 * 1024, 256 * 1024, 400 * 1024, 640 * 1024};
		size_t hheaps[4]{ 1024 * 1024, 2 * 1024 * 1024 , 4 * 1024 * 1024,  8 * 1024 * 1024 };
#endif

		for (auto i = 0u; i < sizeof(sheaps) / sizeof(size_t); i++) {
			if (EC_ALLOCTOR_SHEAP_SIZE / sheaps[i] < 4)
				break;
			_alloctor.add_alloctor(sheaps[i], EC_ALLOCTOR_SHEAP_SIZE / sheaps[i], i < 3);
		}
		for (auto i = 0u; i < sizeof(mheaps) / sizeof(size_t); i++) {
			if (EC_ALLOCTOR_MHEAP_SIZE / mheaps[i] < 4)
				break;
			_alloctor.add_alloctor(mheaps[i], EC_ALLOCTOR_MHEAP_SIZE / mheaps[i], false);
		}
		for (auto i = 0u; i < sizeof(lheaps) / sizeof(size_t); i++) {
			if (EC_ALLOCTOR_HHEAP_SIZE / lheaps[i] < 2)
				break;
			_alloctor.add_alloctor(lheaps[i], EC_ALLOCTOR_HHEAP_SIZE / lheaps[i], false);
		}
		for (auto i = 0u; i < sizeof(hheaps) / sizeof(size_t); i++) {
			if (EC_ALLOCTOR_HHEAP_SIZE < hheaps[i])
				break;
			_alloctor.add_alloctor(hheaps[i], 2, false);
		}
	}
	ec::selfalloctor _alloctor_self;
	ec::allocator _alloctor;
};//ec_allocator_

namespace ec {
	// allocator for std containers
	template <class _Ty>
	class std_allocator {
	public:
		using value_type = _Ty;
		using pointer = _Ty*;
		using reference = _Ty&;
		using const_pointer = const _Ty*;
		using const_reference = const _Ty&;
		using size_type = size_t;
		using difference_type = ptrdiff_t;

		std_allocator() noexcept {
		}
		std_allocator(const std_allocator& alloc) noexcept {
		}
		template <class U>
		std_allocator(const std_allocator<U>& alloc) noexcept {
		}
		~std_allocator() {
		}
		template <class _Other>
		struct  rebind {
			using other = std_allocator<_Other>;
		};
		pointer address(reference x) const noexcept {
			return &x;
		}
		const_pointer address(const_reference x) const noexcept {
			return &x;
		}
		pointer allocate(size_type n, const void* hint = 0) {
			return (pointer)get_ec_allocator()->malloc_(sizeof(value_type) * n);
		}
		void deallocate(pointer p, size_type n) {
			get_ec_allocator()->free_(p);
		}
		size_type max_size() const noexcept {
			return size_t(-1) / sizeof(value_type);
		}
		void construct(pointer p, const_reference val) {
			new ((void*)p) value_type(val);
		}
		template <class _Objty, class... _Types>
		void construct(_Objty* _Ptr, _Types&&... _Args) {
			new ((void*)_Ptr) _Objty(std::forward<_Types>(_Args)...);
		}
		template <class _Uty>
		void destroy(_Uty* const _Ptr) {
			_Ptr->~_Uty();
		}
	};//std_allocator
	template <>
	class std_allocator<void> {
	public:
		using value_type = void;
		typedef void* pointer;
		typedef const void* const_pointer;
		template <class _Other>
		struct  rebind {
			using other = std_allocator<_Other>;
		};
	};
	template <class _Ty, class _Other>
	bool operator==(const std_allocator<_Ty>&, const std_allocator<_Other>&) noexcept {
		return true;
	}
	template <class _Ty, class _Other>
	bool operator!=(const std_allocator<_Ty>&, const std_allocator<_Other>&) noexcept {
		return false;
	}
	template <class OBJ, class... _Types>
	OBJ* newobj(_Types&&... _Args) {
		void* pobj = get_ec_allocator()->malloc_(sizeof(OBJ));
		new ((void*)pobj) OBJ(std::forward<_Types>(_Args)...);
		return (OBJ*)pobj;
	}
	template <class OBJ>
	void delobj(OBJ* pobj) {
		if (pobj) {
			pobj->~OBJ();
			get_ec_allocator()->free_(pobj);
		}
	}
}// namespace ec
inline void* ec_malloc(size_t size, size_t* psize = nullptr) {
	return  get_ec_allocator()->malloc_(size, psize);
}
inline void* ec_realloc(void* ptr, size_t size, size_t* psize = nullptr) {
	return  get_ec_allocator()->realloc_(ptr, size, psize);
}
inline void ec_free(void* ptr) {
	if (ptr)
		get_ec_allocator()->free_(ptr);
}
inline void* ec_calloc(size_t num, size_t size) {
	void* pr = get_ec_allocator()->malloc_(num * size);
	if (pr)
		memset(pr, 0, num * size);
	return pr;
}
inline size_t ec_maxblksize() {
	return get_ec_allocator()->maxblksize();
}
#ifndef _USE_EC_OBJ_ALLOCATOR
#define _USE_EC_OBJ_ALLOCATOR \
static void* operator new(size_t size) {\
	return get_ec_allocator()->malloc_(size);\
}\
static void* operator new(size_t size, void* ptr) {\
	return ptr;\
}\
static void operator delete(void* p) {\
	get_ec_allocator()->free_(p);\
}\
static void operator delete(void* ptr, void* voidptr2) noexcept {\
}
#endif
