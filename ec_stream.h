﻿/*!
\file ec_stream.h
\author	jiangyong
\email  kipway@outlook.com
\update 
  2024-2-1 move vstream to ec_vstream.hpp
stream
	memery stream class

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <stdint.h>
#include <memory.h>
#include <type_traits>
namespace ec
{
	/*!
	*Note: overloaded "<,>" and "<<, >>" do not mix in one line because the priority is different
	*/
	class stream
	{
	public:
		stream() : _pos(0), _size(0), _ps(nullptr)
		{
		};
		stream(void* p, size_t size) : stream()
		{
			attach(p, size);
		};
		~stream() {};

		inline bool is_be()
		{
			union {
				uint32_t u32;
				uint8_t u8;
			} ua;
			ua.u32 = 0x01020304;
			return ua.u8 == 0x01;
		}
	public:
		void attach(void* p, size_t size)
		{
			_ps = (uint8_t*)p;
			_size = size;
			_pos = 0;
		}
		template < typename T,
			class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
			stream & operator >> (T& v) // read as little_endian
		{
			if (_pos + sizeof(T) > _size)
				throw (int)1;
			if (!is_be())
				memcpy(&v, _ps + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t *ps = _ps + _pos, *pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T,
			class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
			stream & operator << (T v) // write as little_endian
		{
			if (_pos + sizeof(T) > _size)
				throw (int)1;
			if (!is_be())
				memcpy(_ps + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t *pd = _ps + _pos, *ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T,
			class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
			stream & operator > (T& v) // read as big_endian
		{
			if (_pos + sizeof(T) > _size)
				throw (int)1;
			if (is_be())
				memcpy(&v, _ps + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t *ps = _ps + _pos, *pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T,
			class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
			stream & operator < (T v)  // write as big_endian
		{
			if (_pos + sizeof(T) > _size)
				throw (int)1;
			if (is_be())
				memcpy(_ps + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t *pd = _ps + _pos, *ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		stream & read(void* pbuf, size_t size)
		{
			if (_pos + size > _size)
				throw (int)1;
			memcpy(pbuf, _ps + _pos, size);
			_pos += size;
			return *this;
		};

		stream & write(const void* pbuf, size_t size)
		{
			if(!pbuf || !size)
				return *this;
			if (_pos + size > _size)
				throw (int)1;
			memcpy(_ps + _pos, pbuf, size);
			_pos += size;
			return *this;
		};

		stream& writeFixedString(const char* s, size_t lenFixed)
		{ // write block to current positiion
			if (_pos + lenFixed > size()) {
				throw (int)1;
			}
			size_t zl = 0;
			if (s && *s) {
				zl = strlen(s);
				if (zl > lenFixed)
					zl = lenFixed;
				memcpy((uint8_t*)_ps + _pos, s, zl);
			}
			if (zl < lenFixed) {
				memset((uint8_t*)_ps + _pos + zl, 0, lenFixed - zl);
			}
			_pos += lenFixed;
			return *this;
		}

		stream & readstr(char* pbuf, size_t size)
		{
			if (!size)
				throw (int)2;
			size_t n = 0;
			while (_pos < _size && _ps[_pos]) {
				if (n + 1 < size) {
					pbuf[n] = _ps[_pos];
					n++;
				}
				_pos++;
			}
			pbuf[n] = 0;
			_pos++;
			return *this;
		};

		stream & writestr(const char* pbuf)
		{
			if (!pbuf || !*pbuf)
				return *this;
			size_t n = 0;
			if (pbuf)
				n = strlen(pbuf);
			if (_pos + n + 1 >= _size)
				throw (int)1;
			if (pbuf && n > 0) {
				memcpy(_ps + _pos, pbuf, n);
				_pos += n;
			}
			_ps[_pos] = 0;
			_pos++;
			return *this;
		};

		stream & setpos(size_t pos)
		{
			if (pos > _size)
				throw (int)1;
			_pos = pos;
			return *this;
		};

		inline size_t getpos() const
		{
			return _pos;
		};
		inline size_t leftsize()
		{
			return _size - _pos;
		}
		inline void* getp()
		{
			return _ps;
		};
		inline bool iseof()
		{
			return _pos == _size;
		}
		inline size_t size() const
		{
			return _size;
		}
	protected:
		size_t	_pos;
		size_t	_size;
		uint8_t* _ps;
	};
};
