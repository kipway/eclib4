/*!
\file ec_vstream.hpp
\author	jiangyong
\email  kipway@outlook.com
\update 
  2024.12.5 add readFixedString() and writeFixedString()
  2024.11.9 support none ec_alloctor
  2023.7.5 add vstream::clear()
  2023.5.15 add vstream

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
#include "ec_string.hpp"
namespace ec
{
	class vstream : public bytes
	{
	protected:
		size_t	_pos{ 0 };  // read write as stream
	public:
		static bool is_be() // is big endian
		{
			union {
				uint32_t u32;
				uint8_t u8;
			} ua;
			ua.u32 = 0x01020304;
			return ua.u8 == 0x01;
		}
		using bytes::bytes;
		vstream(size_t sizereserve = 0) : _pos(0) {
			if (sizereserve)
				reserve(sizereserve);
		}
		vstream& setpos(size_t pos) noexcept
		{
			size_t usize = size();
			_pos = pos > usize ? usize : pos;
			return *this;
		}
		inline void clear() {
			_pos = 0;
			bytes::clear();
		}
		inline void postoend() noexcept
		{
			_pos = size();
		}

		inline size_t getpos() const noexcept
		{
			return _pos;
		}

		vstream& read(void* pdata, size_t rsize)
		{ // read block from current positiion
			if (_pos + rsize > size()) {
				throw std::range_error("oversize");
				return *this;
			}
			memcpy(pdata, data() + _pos, rsize);
			_pos += rsize;
			return *this;
		}

		template<class STR = std::string, class = typename std::enable_if<std::is_class<STR>::value>::type>
		vstream& readFixedString(STR& sout, size_t lenFixed)
		{
			sout.clear();
			if (_pos + lenFixed > size()) {
				throw std::range_error("oversize");
				return *this;
			}
			sout.resize(lenFixed);
			memcpy((void*)sout.data(), data() + _pos, lenFixed);
			_pos += lenFixed;
			auto i = 0u;
			while (i < lenFixed) {
				if (!sout[i]) {
					sout.resize(i);
					break;
				}
				++i;
			}
			return *this;
		}

		vstream& write(const void *pdata, size_t wsize)
		{ // write block to current positiion
			if (_pos + wsize > size()) {
				resize(_pos + wsize);
			}
			memcpy(data() + _pos, pdata, wsize);
			_pos += wsize;
			return *this;
		}

		vstream& writeFixedString(const char* s, size_t lenFixed)
		{ // write block to current positiion
			if (_pos + lenFixed > size()) {
				resize(_pos + lenFixed);
			}
			size_t zl = 0;
			if (s && *s) {
				zl = strlen(s);
				if (zl > lenFixed)
					zl = lenFixed;
				memcpy((uint8_t*)data() + _pos, s, zl);
			}
			if (zl < lenFixed) {
				memset((uint8_t*)data() + _pos + zl, 0, lenFixed - zl);
			}
			_pos += lenFixed;
			return *this;
		}

		template<class STR = std::string, class = typename std::enable_if<std::is_class<STR>::value>::type>
		vstream& writeFixedString(STR s, size_t lenFixed)
		{ // write block to current positiion
			if (_pos + lenFixed > size()) {
				resize(_pos + lenFixed);
			}
			size_t zl = s.size();
			if (zl > lenFixed)
				zl = lenFixed;
			if (zl) {
				memcpy((uint8_t*)data() + _pos, s.data(), zl);
			}
			if (zl < lenFixed) {
				memset((uint8_t*)data() + _pos + zl, 0, lenFixed - zl);
			}
			_pos += lenFixed;
			return *this;
		}

		template < typename T, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		vstream& operator >> (T& v)
		{ // read as little_endian from current positiion
			if (_pos + sizeof(T) > size())
				throw std::range_error("oversize");
			if (!is_be())
				memcpy(&v, data() + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* ps = (uint8_t*)data() + _pos, * pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		vstream& operator > (T& v)
		{ // read as big_endian from current positiion
			if (_pos + sizeof(T) > size())
				throw std::range_error("oversize");
			if (is_be())
				memcpy(&v, data() + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* ps = (uint8_t*)data() + _pos, * pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		vstream& operator << (T v)
		{ // write as little_endian to current positiion
			if (_pos + sizeof(T) > size())
				resize(_pos + sizeof(T));
			if (!is_be())
				memcpy(data() + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* pd = (uint8_t*)data() + _pos, * ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
		vstream& operator < (T v)
		{ // write as big_endian to current positiion
			if (_pos + sizeof(T) > size())
				resize(_pos + sizeof(T));
			if (is_be())
				memcpy(data() + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* pd = (uint8_t*)data() + _pos, * ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}
	};
};
