/*!
\file ec_string.h
\author	jiangyong
\email  kipway@outlook.com
\update 
  2024.11.11 add chararray and bytearray
  2024.2.26 remove ec::array, replace ec::strxxx with EC_STACKSTRING
  2024.2.1  remove include ec::string.hpp. add ec::fixstring_ from ec::string.hpp, not need ec_allocator.h
  2023.5.25 Support RFC8259 full JSON escaping
  2023.5.15 move ec::string to ec_string.hpp

string tips

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <memory.h>
#include <string>
#include <ctype.h>
#include <type_traits>
#include <stdarg.h>
#ifndef _WIN32
#include <iconv.h>
#endif

#define _to_upper(a) (((a) >= 'a' && (a) <= 'z') ? (a)-32 : (a))	// ('a'-'A') = 32
#define _to_lower(a) (((a) >= 'A' && (a) <= 'Z') ? (a)+32 : (a))	// ('a'-'A') = 32

#include "ec_text.h"

#define WIN_CP_GBK  936
#define WIN_CP_UTF8 65001
namespace ec
{
	inline int stricmp(const char*s1, const char*s2)
	{
		if (s1 == s2)
			return 0;
#ifdef _WIN32
		return ::_stricmp(s1, s2);
#else
		return ::strcasecmp(s1, s2);
#endif
	}

	inline 	size_t strlcpy(char* sd, const char* ss, size_t count)
	{// like strlcpy for linux,add null to the end of sd,return strlen(ss)
		if (!ss || !(*ss)) {
			if (sd && count)
				*sd = '\0';
			return 0;
		}
		const size_t srclen = strlen(ss);
		if (!sd || !count)
			return srclen;

		if (srclen < count)
			memcpy(sd, ss, srclen + 1);
		else {
			memcpy(sd, ss, count - 1);
			sd[count - 1] = '\0';
		}
		return srclen;
	}

	inline char *strncpy_s(char *dest, size_t destsize, const char* src, size_t srcsize)
	{ // safe copy, always adding a null character at the end of dest
		if (!dest || !destsize)
			return dest;
		if (!src || !srcsize) {
			*dest = '\0';
			return dest;
		}

		if (destsize <= srcsize)
			srcsize = destsize - 1;
		memcpy(dest, src, srcsize);
		dest[srcsize] = '\0';
		return dest;
	}

	inline bool streq(const char* s1, const char* s2)
	{ // return true: equal; false not equal
		if (!s1 || !s2)
			return false;
		while (*s1 && *s2) {
			if (*s1++ != *s2++)
				return false;
		}
		return *s1 == '\0' && *s2 == '\0';
	}

	inline bool strieq(const char* s1, const char* s2)
	{ //case insensitive equal. return true: equal; false not equal
		if (s1 == s2)
			return true;
		if (!s1 || !s2)
			return false;

#ifdef _WIN32
			return (stricmp(s1, s2) == 0);
#else
			return (strcasecmp(s1, s2) == 0);
#endif
	}

	inline int strineq_(const char* s1, const char* s2, int n)// return n characters equal case insensitive
	{
		if (!s1 || !s2)
			return 0;
		int i = 0;
		while (*s1 && *s2 && i < n) {
			if (*s1 != *s2 && tolower(*s1) != tolower(*s2))
				break;
			++s1;
			++s2;
			++i;
		}
		return i;
	}

	inline bool strineq(const char* s1, const char* s2, size_t s2size, size_t  zsize)// Judge zsize characters equal case insensitive
	{
		if (!s1 || !s2 || s2size < zsize)
			return false;
		size_t i = 0;
		while (i < s2size && i < zsize && *s1) {
			if (*s1 != *s2 && tolower(*s1) != tolower(*s2))
				return false;
			++s1;
			++s2;
			++i;
		}
		return i == zsize;
	}

	inline void strtrim(char *s, const char* flt = "\x20\t\n\r")
	{
		if (!s || !*s)
			return;
		char *sp = s, *s1 = s;
		while (*sp && strchr(flt, *sp))
			sp++;
		if (sp != s) {
			while (*sp)
				*s1++ = *sp++;
			*s1 = '\0';
		}
		else
			while (*s1++);
		while (s1 > s) {
			s1--;
			if (strchr(flt, *s1))
				*s1 = '\0';
			else
				break;
		}
	}
	inline char* strtrimright(char* s, const char* flt = "\x20\t\n\r")
	{
		if (!s || !*s)
			return s;
		char* s1 = s;
		while (*s1++);
		char* se = s1 - 1;
		while (s1 > s) {
			s1--;
			if (!strchr(flt, *s1)) {
				if (s1 != se)
					*(s1 + 1) = '\0';
				return s;
			}
		}
		return s;
	}
	/*!
		\brief get next string
		\param cp separate character
		\param src source string
		\param srcsize source string length
		\param pos [in/out] current position
		\param sout [out] output buffer
		\param outsize output buffer length
	*/
	inline const char* strnext(const char cp, const char* src, size_t srcsize, size_t &pos, char *sout, size_t outsize)
	{
		char c;
		size_t i = 0;
		while (pos < srcsize) {
			c = src[pos++];
			if (c == cp) {
				while (i > 0) { // delete tail space char
					if (sout[i - 1] != '\t' && sout[i - 1] != ' ')
						break;
					i--;
				}
				sout[i] = '\0';
				if (i > 0)
					return sout;
			}
			else if (c != '\n' && c != '\r') {
				if (i == 0 && (c == '\t' || c == ' ')) //delete head space char
					continue;
				sout[i++] = c;
				if (i >= outsize)
					return nullptr;
			}
		}
		if (i && i < outsize && pos == srcsize) {
			while (i > 0) { //delete tail space char
				if (sout[i - 1] != '\t' && sout[i - 1] != ' ')
					break;
				i--;
			}
			sout[i] = '\0';
			if (i > 0)
				return sout;
		}
		return nullptr;
	}

	/*!
	\brief get next string
	\param split separate characters
	\param src source string
	\param srcsize source string length
	\param pos [in/out] current position
	\param sout [out] output buffer
	\param outsize output buffer length
	*/

	inline const char* strnext(const char* split, const char* src, size_t srcsize, size_t &pos, char *sout, size_t outsize)
	{
		char c;
		size_t i = 0;
		while (pos < srcsize) {
			c = src[pos++];
			if (strchr(split, c)) {
				while (i > 0) { // delete tail space char
					if (sout[i - 1] != '\t' && sout[i - 1] != ' ')
						break;
					i--;
				}
				sout[i] = '\0';
				if (i > 0)
					return sout;
			}
			else if (c != '\n' && c != '\r') {
				if (i == 0 && (c == '\t' || c == ' ')) //delete head space char
					continue;
				sout[i++] = c;
				if (i >= outsize)
					return nullptr;
			}
		}
		if (i && i < outsize && pos == srcsize) {
			while (i > 0) { //delete tail space char
				if (sout[i - 1] != '\t' && sout[i - 1] != ' ')
					break;
				i--;
			}
			sout[i] = '\0';
			if (i > 0)
				return sout;
		}
		return nullptr;
	}

	inline bool char2hex(char c, unsigned char *pout)
	{
		if (c >= 'a' && c <= 'f')
			*pout = 0x0a + (c - 'a');
		else if (c >= 'A' && c <= 'F')
			*pout = 0x0a + (c - 'A');
		else if (c >= '0' && c <= '9')
			*pout = c - '0';
		else
			return false;
		return true;
	}

	template<class  _Str = std::string>
	int url2utf8(const char* url, _Str &so)
	{ //utf8 fomat url translate to utf8 string
		unsigned char h, l;
		so.clear();
		while (*url) {
			if (*url == '%') {
				url++;
				if (!char2hex(*url++, &h))
					break;
				if (!char2hex(*url++, &l))
					break;
				so += (char)((h << 4) | l);
			}
			else if (*url == '+') {
				so += '\x20';
				url++;
			}
			else
				so += *url++;
		}
		return (int)so.size();
	}

	template<class _Str = std::string>
	int utf82url(const char* url, _Str &so)
	{ //utf8 string -> url
		unsigned char h, l, *p = (unsigned char*)url;
		so.clear();
		while (*p) {
			if (*p == '\x20') {
				so += '+';
				p++;
			}
			else if (*p & 0x80) {
				so += '%';
				h = (*p & 0xF0) >> 4;
				l = *p & 0x0F;
				if (h >= 10)
					so += ('A' + h - 10);
				else
					so += ('0' + h);
				if (l >= 10)
					so += ('A' + l - 10);
				else
					so += ('0' + l);
				p++;
			}
			else
				so += (char)* p++;
		}
		return (int)so.size();
	}

	inline 	char* strupr(char *str)
	{
#ifdef _WIN32
		return _strupr(str);
#else
		char *ptr = str;
		while (*ptr) {
			if (*ptr >= 'a' && *ptr <= 'z')
				*ptr -= 'a' - 'A';
			ptr++;
		}
		return str;
#endif
	}

	inline char* strlwr(char *str)
	{
#ifdef _WIN32
		return _strlwr(str);
#else
		char *ptr = str;
		while (*ptr) {
			if (*ptr >= 'A' && *ptr <= 'Z')
				*ptr += 'a' - 'A';
			ptr++;
		}
		return str;
#endif
	}
	
	inline size_t struppercpy(char* sd, const char* ss, size_t count)
	{// like strlcpy for linux,add null to the end of sd,return strlen(ss)
		char* d = sd;
		const char* s = ss;
		size_t n = count;
		char ch;

		if (!d)
			return 0;

		if (!s) {
			*d = 0;
			return 0;
		}
		/* Copy as many bytes as will fit */
		if (n != 0 && --n != 0) {
			do {
				ch = *s++;
				if ((*d++ = _to_upper(ch)) == 0)
					break;
			} while (--n != 0);
		}

		/* Not enough room in dst, add NUL and traverse rest of src */
		if (n == 0) {
			if (count != 0)
				*d = '\0';  /* NUL-terminate dst */
			while (*s++)
				;
		}

		return(s - ss - 1); /* count does not include NUL */
	}

	inline size_t strlowercpy(char* sd, const char* ss, size_t count)
	{// like strlcpy for linux,add null to the end of sd,return strlen(ss)
		char* d = sd;
		const char* s = ss;
		size_t n = count;
		char ch;

		if (!d)
			return 0;

		if (!s) {
			*d = 0;
			return 0;
		}

		/* Copy as many bytes as will fit */
		if (n != 0 && --n != 0) {
			do {
				ch = *s++;
				if ((*d++ = _to_lower(ch)) == 0)
					break;
			} while (--n != 0);
		}

		/* Not enough room in dst, add NUL and traverse rest of src */
		if (n == 0) {
			if (count != 0)
				*d = '\0';   /* NUL-terminate dst */
			while (*s++)
				;
		}
		return(s - ss - 1);  /* count does not include NUL */
	}

	inline bool strisutf8(const char* s, size_t size = 0)
	{ //return true if s is utf8 string
		if (!s)
			return true;
		uint8_t c;
		int nb = 0;
		const char* pend = s + (size ? size : strlen(s));
		while (s < pend) {
			c = *s++;
			if (!nb) {
				if (!(c & 0x80))
					continue;
				if (0xc0 == c || 0xc1 == c || c > 0xF4) // RFC 3629
					return false;
				if ((c & 0xFC) == 0xFC) // 1111 1100
					nb = 5;
				else if ((c & 0xF8) == 0xF8) // 1111 1000
					nb = 4;
				else if ((c & 0xF0) == 0xF0) // 1111 0000
					nb = 3;
				else if ((c & 0xE0) == 0xE0) // 1110 1000
					nb = 2;
				else if ((c & 0xC0) == 0xC0) // 1100 0000
					nb = 1;
				else
					return false;
				continue;
			}
			if ((c & 0xC0) != 0x80)
				return false;
			nb--;
		}
		return !nb;
	}

	inline bool strisascii(const char* s, size_t size = 0)
	{// If s is a pure ASCII code or a utf8 string containing only ASCII, it will return true
		if (!s)
			return true;
		const char* pend = s + (size ? size : strlen(s));
		while (s < pend) {
			if (*s & 0x80)
				return false;
			++s;
		}
		return true;
	}

	inline int utf82gbk(const char* in, size_t sizein, char *out, size_t sizeout)
	{ //return number bytes write to out or -1 error
		*out = 0;
		if (!in || !(*in))
			return 0;
#ifdef _WIN32
		wchar_t wtmp[16384];
		wchar_t* sUnicode = wtmp;
		int sizetmp = 16383, nret = -1;
		for (;;) {
			if (sizein > (size_t)sizetmp) {
				sizetmp = MultiByteToWideChar(WIN_CP_UTF8, 0, in, (int)sizein, NULL, 0);
				if (!sizetmp)
					break;
				sUnicode = (wchar_t*)ec::g_malloc(sizeof(wchar_t) * (sizetmp + 1));
				if (!sUnicode)
					break;
			}
			sizetmp = MultiByteToWideChar(WIN_CP_UTF8, 0, in, (int)sizein, sUnicode, sizetmp); //utf8 -> unicode wchar
			if (!sizetmp)
				break;
			sizetmp = WideCharToMultiByte(WIN_CP_GBK, 0, sUnicode, sizetmp, out, (int)sizeout - 1, NULL, NULL); //unicode wchar -> gbk
			if (sizetmp) {
				out[sizetmp] = 0;
				nret = (int)sizetmp;
			}
			break;
		}
		if (sUnicode != wtmp)
			ec::g_free(sUnicode);
		return nret;
#else
		iconv_t cd;
		char **pin = (char**)&in;
		char **pout = &out;

		cd = iconv_open("GBK//IGNORE", "UTF-8//IGNORE");
		if (cd == (iconv_t)-1)
			return -1;

		size_t inlen = sizein;
		size_t outlen = sizeout - 1;
		if (iconv(cd, pin, &inlen, pout, &outlen) == (size_t)(-1)) {
			iconv_close(cd);
			return -1;
		}
		iconv_close(cd);
		*out = 0;
		return (int)(sizeout - outlen - 1);
#endif
	}

	inline int gbk2utf8(const char* in, size_t sizein, char *out, size_t sizeout)
	{ //return number bytes write to out or -1 error
		*out = 0;
		if (!in || !(*in))
			return 0;
#ifdef _WIN32
		wchar_t wtmp[16384];
		wchar_t* sUnicode = wtmp;
		int sizetmp = 16383, nret = -1;
		for (;;) {
			if (sizein > (size_t)sizetmp) {
				sizetmp = MultiByteToWideChar(WIN_CP_GBK, 0, in, (int)sizein, NULL, 0);
				if (!sizetmp)
					break;
				sUnicode = (wchar_t*)ec::g_malloc(sizeof(wchar_t) * (sizetmp + 1));
				if (!sUnicode)
					break;
			}
			sizetmp = MultiByteToWideChar(WIN_CP_GBK, 0, in, (int)sizein, sUnicode, sizetmp); //gbk -> unicode wchar
			if (!sizetmp)
				break;
			sizetmp = WideCharToMultiByte(WIN_CP_UTF8, 0, sUnicode, sizetmp, out, (int)sizeout - 1, NULL, NULL); //unicode wchar -> utf-8
			if (sizetmp) {
				out[sizetmp] = 0;
				nret = (int)sizetmp;
			}
			break;
		}
		if (sUnicode != wtmp)
			ec::g_free(sUnicode);
		return nret;
#else
		iconv_t cd;
		char **pin = (char**)&in;
		char **pout = &out;

		cd = iconv_open("UTF-8//IGNORE", "GBK//IGNORE");
		if (cd == (iconv_t)-1)
			return -1;
		size_t inlen = sizein;
		size_t outlen = sizeout - 1;
		if (iconv(cd, pin, &inlen, pout, &outlen) == (size_t)(-1)) {
			iconv_close(cd);
			return -1;
		}
		iconv_close(cd);
		*out = 0;
		return (int)(sizeout - outlen - 1);
#endif
	}

	template <class _Str = std::string>
	int gbk2utf8_s(const char* in, size_t sizein, _Str &sout)
	{ // return sout.zize() or -1 error;
		if (sizein * 3 >= 16384)
			return -1;
		char tmp[16384];
		int n = gbk2utf8(in, sizein, tmp, sizeof(tmp));
		if (n < 0)
			return -1;
		try {
			sout.append(tmp, n);
		}
		catch (...) {
			return -1;
		}
		return n;
	}

	template <class _Str = std::string>
	int gbk2utf8_s(_Str &s) // return s.zize() or -1 error;
	{
		if (s.empty() || strisutf8(s.data(), s.size()))
			return (int)s.size();
		if (s.size() * 3 >= 16384)
			return -1;
		char tmp[16384];
		int n = gbk2utf8(s.data(), s.size(), tmp, sizeof(tmp));
		if (n < 0)
			return -1;
		try {
			s.assign(tmp, n);
		}
		catch (...) {
			return -1;
		}
		return n;
	}

	template <class _Str = std::string>
	int utf82gbk_s(const char* in, size_t sizein, _Str &sout)
	{
		if (sizein >= 16384)
			return -1;
		char tmp[16384];
		int n = utf82gbk(in, sizein, tmp, sizeof(tmp));
		if (n < 0)
			return -1;
		try {
			sout.append(tmp, n);
		}
		catch (...) {
			return -1;
		}
		return n;
	}

	inline void hex2str(const void* psrc, size_t sizesrc, char *sout, size_t outsize)
	{
		unsigned char uc;
		size_t i;
		const unsigned char* pu = (const unsigned char*)psrc;
		for (i = 0; i < sizesrc && 2 * i + 2 < outsize; i++) {
			uc = pu[i] >> 4;
			sout[i * 2] = (uc >= 0x0A) ? 'A' + (uc - 0x0A) : '0' + uc;
			uc = pu[i] & 0x0F;
			sout[i * 2 + 1] = (uc >= 0x0A) ? 'A' + (uc - 0x0A) : '0' + uc;
		}
		sout[2 * i] = 0;
	}

	template <class _Str = std::string>
	inline void hex2str(const void* psrc, size_t sizesrc, _Str& sout)
	{
		unsigned char uc;
		size_t i;
		const unsigned char* pu = (const unsigned char*)psrc;
		for (i = 0; i < sizesrc; i++) {
			uc = pu[i] >> 4;
			sout.push_back((uc >= 0x0A) ? 'A' + (uc - 0x0A) : '0' + uc);
			uc = pu[i] & 0x0F;
			sout.push_back((uc >= 0x0A) ? 'A' + (uc - 0x0A) : '0' + uc);
		}
	}

	inline void xor_le(unsigned char* pd, int size, unsigned int umask)
	{ // little endian fast XOR,4x faster than byte-by-byte XOR
		if (!size)
			return;
		int i = 0, nl = 0, nu;
		unsigned int um = umask;
		if ((size_t)pd % 4) {
			nl = 4 - ((size_t)pd % 4);
			um = umask >> nl * 8;
			um |= umask << (4 - nl) * 8;
		}
		nu = (size - nl) / 4;
		for (i = 0; i < nl && i < size; i++)
			pd[i] ^= (umask >> ((i % 4) * 8)) & 0xFF;

		unsigned int *puint = (unsigned int*)(pd + i);
		for (i = 0; i < nu; i++)
			puint[i] ^= um;

		for (i = nl + nu * 4; i < size; i++)
			pd[i] ^= (umask >> ((i % 4) * 8)) & 0xFF;
	}

	inline int hexview16(const void* psrc, int srclen, char * sout, size_t sizeout, size_t *pzoutsize = nullptr)
	{ //view 16 bytes，return do bytes
		if (pzoutsize)
			*pzoutsize = 0;
		*sout = 0;
		if (srclen <= 0 || sizeout < 88u)
			return -1;
		int i, k = 0, n = srclen > 16 ? 16 : srclen;
		unsigned char ul, uh;
		const unsigned char* s = (const unsigned char*)psrc;
		sout[k++] = '\x20';
		sout[k++] = '\x20';
		for (i = 0; i < 16; i++) {
			if (i < n) {
				uh = (s[i] & 0xF0) >> 4;
				ul = (s[i] & 0x0F);

				if (uh < 10)
					sout[k++] = '0' + uh;
				else
					sout[k++] = 'A' + uh - 10;
				if (ul < 10)
					sout[k++] = '0' + ul;
				else
					sout[k++] = 'A' + ul - 10;
			}
			else {
				sout[k++] = '\x20';
				sout[k++] = '\x20';
			}
			sout[k++] = '\x20';
			if (i == 7 || i == 15) {
				sout[k++] = '\x20';
				sout[k++] = '\x20';
				sout[k++] = '\x20';
			}
		}
		for (i = 0; i < n; i++) {
			if (isprint(s[i]))
				sout[k++] = s[i];
			else
				sout[k++] = '.';
		}
		sout[k++] = '\n';
		sout[k] = '\0';
		if (pzoutsize)
			*pzoutsize = k;
		return n;
	}

	/*!
	\brief view bytes
	like this
	16 03 03 00 33 01 00 00    2F 03 03 39 7F 29 AE 20    ....3.../..9.).
	8D 03 12 61 52 0A 2E 02    86 13 66 CA 3C 7E 6A 54    ...aR.....f.<~jT
	39 D2 CD 22 D6 A7 2C 08    EF F4 BC 00 00 08 00 3D    9.."..,........=
	00 3C 00 35 00 2F 01 00                               .<.5./..
	*/
	inline const char* bin2view(const void* pm, size_t size, char *so, size_t sizeout)
	{
		if (!so)
			return nullptr;
		*so = 0;
		if (sizeout < 6 || !size)
			return so;
		char stmp[256];
		int ndo = 0, n = (int)size, nd;
		size_t zlen = 0, zall = 0;
		const uint8_t* p = (uint8_t*)pm;
		while (n - ndo > 0) {
			nd = ec::hexview16(p + ndo, n - ndo, stmp, sizeof(stmp), &zlen);
			if (nd <= 0)
				break;
			if (zall + zlen >= sizeout)
				break;
			memcpy(so + zall, stmp, zlen);
			zall += zlen;
			ndo += nd;
		}
		*(so + zall) = 0;
		return so;
	}

	/*!
	\brief view bytes
	like this
	0000-000F 16 03 03 00 33 01 00 00    2F 03 03 39 7F 29 AE 20    ....3.../..9.).
	0010-001F 8D 03 12 61 52 0A 2E 02    86 13 66 CA 3C 7E 6A 54    ...aR.....f.<~jT
	0020-002F 39 D2 CD 22 D6 A7 2C 08    EF F4 BC 00 00 08 00 3D    9.."..,........=
	0030-003F 00 3C 00 35 00 2F 01 00                               .<.5./..
	*/
	template<class _STR = std::string>
	void bin2view(const void* pm, size_t size, _STR& sout) {
		int ndo = 0, nall = size, n;
		size_t zr;
		const uint8_t* pu = (const uint8_t*)pm;
		char stmp[256];
		while (ndo < nall) {
			zr = snprintf(stmp, sizeof(stmp), "%04X-%04X", ndo, ndo + 15);
			sout.append(stmp, zr);
			n = ec::hexview16(pu + ndo, nall - ndo, stmp, sizeof(stmp), &zr);
			if (n <= 0)
				break;
			sout.append(stmp, zr);
			ndo += n;
		}
	}

	template<class _STR = std::string>
	void formatpath(_STR &s)
	{
		if (s.empty())
			return;
		for (auto &i : s) {
			if (i == '\\')
				i = '/';
		}

		if (s.back() != '/')
			s.push_back('/');
	}

	template<class _Out>
	int strsplit(const char* split, const char* src, size_t srcsize, _Out& out, int maxitems = 0)
	{ // return	number of t_str in out
		out.clear();
		txt t = { src,0 };
		const char* send = src + srcsize;
		while (src < send) {
			if (strchr(split, *src)) {
				if ((size_t)src > (size_t)t._str) {
					t._size = src - t._str;
					out.push_back(t);
					if (maxitems && (int)out.size() >= maxitems)
						return (int)out.size();
				}
				t._str = ++src;
				t._size = 0;
			}
			else
				++src;
		}
		if ((size_t)src > (size_t)t._str && (!maxitems || (int)out.size() < maxitems)) {
			t._size = src - t._str;
			out.push_back(t);
		}
		return (int)out.size();
	}

	/*!
		\brief filter string

		sfliter support *?
		\param ssrc [in] src
		\param sfliter [in] filter str
		\return true success
		*/
	inline bool strfilter(const char *ssrc, const char *sfliter, char mchar = '*', char schar = '?')
	{
		char ssub[512], cp = 0;
		char *ps = ssub, *ss = (char *)ssrc, *sf = (char *)sfliter;
		if (!ss || !sf || *sf == 0)
			return true;
		if ((*sf == mchar) && (*(sf + 1) == 0))
			return true;
		while ((*sf) && (*ss)) {
			if (*sf == mchar) {
				if (ps != ssub) {
					*ps = 0;
					ss = strstr(ss, ssub);
					if (!ss)
						return false;
					ss += (ps - ssub);
					ps = ssub;
				}
				cp = mchar;
				sf++;
			}
			else if (*sf == schar) {
				if (ps != ssub) {
					*ps = 0;
					ss = strstr(ss, ssub);
					if (!ss)
						return false;
					ss += (ps - ssub);
					ps = ssub;
				}
				ps = ssub;
				cp = schar;
				ss++;
				sf++;
			}
			else {
				if (cp == mchar)
					*ps++ = *sf++;
				else {
					if (*sf != *ss)
						return false;
					sf++;
					ss++;
				}
			}
		}//while
		if (cp != mchar) {
			if (*ss == *sf)
				return true;
			if (*sf == mchar) {
				sf++;
				if (*sf == 0)
					return true;
			}
			return false;
		}
		if (ps != ssub) {
			*ps = 0;
			ss = strstr(ss, ssub);
			if (!ss)
				return false;
			ss += (ps - ssub);
			if (!*ss)
				return true;
			return false;
		}
		return true;
	}

	template<class _STR = std::string>
	size_t utf8_substr(_STR &s, size_t sublen) // return substr size
	{
		if (s.size() <= sublen)
			return s.size();
		if (s.empty())
			return 0;
		uint8_t uc;
		size_t pos = s.size() - 1;
		while (pos > 0) {
			uc = s[pos];
			if ((uc < 0x80 || uc >= 0xC0) && pos <= sublen)
				break;
			--pos;
		}
		s.resize(pos);
		return pos;
	}

	inline size_t utf8_substr(char *s, size_t size, size_t sublen)
	{ // truncate string no greater than sublen, return substr size
		if (size <= sublen)
			return size;
		if (!s || !*s || !size)
			return 0;
		uint8_t uc;
		size_t pos = size - 1;
		while (pos > 0) {
			uc = s[pos];
			if ((uc < 0x80 || uc >= 0xC0) && pos <= sublen)// uc >= 0xC0 mean the first utf8 byte
				break;
			--pos;
		}
		s[pos] = 0;
		return pos;
	}

	inline size_t utf8_sizesubstr(const char* s, size_t size, size_t sublen)
	{ // return substr size no greater than sublen
		if (size < sublen)
			return size;
		if (!s || !*s || !size)
			return 0;
		uint8_t uc;
		size_t pos = size - 1;
		while (pos > 0) {
			uc = s[pos];
			if ((uc < 0x80 || uc >= 0xC0) && pos <= sublen) // uc >= 0xC0 mean the first utf8 byte
				break;
			--pos;
		}
		return pos;
	}

	inline size_t utf8cpy(char* sd, size_t sized, const char* ss, size_t sizes)
	{ //add null to end, return copy size
		if (!ss || !(*ss)) {
			if (sd && sized)
				*sd = '\0';
			return 0;
		}
		if (!sd || !sized)
			return 0;
		size_t zcp = utf8_sizesubstr(ss, sizes, sized);
		if (zcp)
			memcpy(sd, ss, zcp);
		sd[zcp] = 0;
		return zcp;
	}

	inline size_t utf8_strlcpy(char* sd, const char* ss, size_t count, size_t sslen = 0)
	{// like strlcpy for linux,add null to the end of sd,return strlen(ss), count is sd size
		if (!ss || !(*ss)) {
			if (sd && count)
				*sd = '\0';
			return 0;
		}
		size_t srclen = sslen ? sslen : strlen(ss);
		if (!sd || !count)
			return srclen;

		size_t zcp = utf8_sizesubstr(ss, srclen, count);
		if (zcp)
			memcpy(sd, ss, zcp);
		sd[zcp] = 0;
		return srclen;
	}

	inline bool jstr_needesc(const char* src, size_t srcsize)
	{
		for (auto i = 0u; i < srcsize; i++) {
			switch (src[i]) {
			case '\b':
			case '\t':
			case '\n':
			case '\r':
			case '\f':
			case '\"':
			case '\\':
			case '/':
				return true;
				break;
			}
		}
		return false;
	}

	template<class _STR>
	void outJsonEsc(char c, _STR& sout)
	{
		switch (c) {
		case '\b':
			sout += '\\';
			sout += 'b';
			break;
		case '\t':
			sout += '\\';
			sout += 't';
			break;
		case '\n':
			sout += '\\';
			sout += 'n';
			break;
		case '\r':
			sout += '\\';
			sout += 'r';
			break;
		case '\f':
			sout += '\\';
			sout += 'f';
			break;
		case '\"':
		case '/':
		case '\\':
			sout += '\\';
			sout += c;
			break;
		default:
			sout += c;
		}
	}

	template<typename _Str>
	const char* jstr_toesc(const char* s, size_t srcsize, _Str &so)
	{ //escape  '\' -> '\\', '"' -> '\"'
		so.clear();
		const char *se = s + srcsize;
		while (s < se) {
			outJsonEsc(*s++, so);
		}
		return so.c_str();
	}

	template<typename _Str>
	void Unicode2Utf8(uint32_t ucode, _Str& sout) {
		using etp = typename _Str::value_type;
		if (ucode < 0x80)// 0xxxxxxx
			sout += static_cast<etp>(ucode);
		else if (ucode < 0x800) {  // 110xxxxx 10xxxxxx
			sout += static_cast<etp>(((ucode >> 6) & 0x1F) | 0xC0);
			sout += static_cast<etp>(ucode & 0x3F) | 0x80;
		}
		else if (ucode < 0x10000) {// 1110xxxx 10xxxxxx 10xxxxxx
			sout += static_cast<etp>(((ucode >> 12) & 0x0F) | 0xE0);
			sout += static_cast<etp>(((ucode >> 6) & 0x3F) | 0x80);
			sout += static_cast<etp>(ucode & 0x3F) | 0x80;
		}
		else if (ucode < 0x200000) {//11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
			sout += static_cast<etp>(((ucode >> 18) & 0x07) | 0xF0);
			sout += static_cast<etp>(((ucode >> 12) & 0x3F) | 0x80);
			sout += static_cast<etp>(((ucode >> 6) & 0x3F) | 0x80);
			sout += static_cast<etp>(ucode & 0x3F) | 0x80;
		}
		else if (ucode < 0x4000000) {//111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
			sout += static_cast<etp>(((ucode >> 24) & 0x03) | 0xF8);
			sout += static_cast<etp>(((ucode >> 18) & 0x3F) | 0x80);
			sout += static_cast<etp>(((ucode >> 12) & 0x3F) | 0x80);
			sout += static_cast<etp>(((ucode >> 6) & 0x3F) | 0x80);
			sout += static_cast<etp>(ucode & 0x3F) | 0x80;
		}
		else { //1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
			sout += static_cast<etp>(((ucode >> 30) & 0x01) | 0xFC);
			sout += static_cast<etp>(((ucode >> 24) & 0x3F) | 0x80);
			sout += static_cast<etp>(((ucode >> 18) & 0x3F) | 0x80);
			sout += static_cast<etp>(((ucode >> 12) & 0x3F) | 0x80);
			sout += static_cast<etp>(((ucode >> 6) & 0x3F) | 0x80);
			sout += static_cast<etp>(ucode & 0x3F) | 0x80;
		}
	}

	/**
	 * @brief 解析转义JSON utf-16
	 * @param s 输入字符串, "\u0027"或者 "\uD950\uDF21"
	 * @param len 字符串长度
	 * @param ucodeout 输出转码后unicode
	 * @return -1：错误; >=0 成功转义消耗的字节数。
	*/
	inline int parseJsonUtf16(const char* s, size_t len, uint32_t& ucodeout)
	{
		if (len < 6)
			return -1;
		uint8_t uc = 0;
		uint32_t ucode = 0;
		for (auto i = 0; i < 4; i++) {
			if (!char2hex(s[i + 2], &uc))
				return -1;
			ucode = i ? (ucode << 4) + uc : uc;
		}
		if (ucode >= 0xD800 && ucode <= 0xDBFF) { //UTF-16高代理, 高10位
			if (len < 12)
				return -1;
			uint32_t ulow = 0;
			for (auto i = 0; i < 4; i++) { //继续解析低代理, 低10位
				if (!char2hex(s[i + 8], &uc))
					return -1;
				ulow = i ? (ulow << 4) + uc : uc;
			}
			if (ulow < 0xDC00 || ucode > 0xDFFF) //不在低代理返回结束转义返回。
				return -1;
			ucodeout = ((ucode & 0x3FF) << 10) | (ulow & 0x3FF);
			return 12;
		}
		ucodeout = ucode;
		return 6;
	}

	template<typename _Str>
	const char* jstr_fromesc(const char* s, size_t srcsize, _Str &so)
	{ // delete escape, "\\" -> '\', ""\'" -> '"'  so
		so.clear();
		if (!s || !srcsize)
			return so.c_str();
		bool besc = false;
		for (auto i = 0u; i < srcsize; i++) {
			if (s[i] == '\\') {
				besc = true;
				break;
			}
		}
		if (!besc) {
			so.append(s, srcsize);
			return so.c_str();
		}
		so.reserve(srcsize);
		int nchars;
		uint32_t ucode;
		const char* se = s + srcsize;
		while (s < se) {
			if (*s == '\\') {
				if (s + 1 >= se)
					break;
				switch (*(s + 1)) {
				case '\"':
				case '/':
				case '\\':
					so += *(s + 1);
					break;
				case 'b':
					so += '\b';
					break;
				case 't':
					so += '\t';
					break;
				case 'r':
					so += '\r';
					break;
				case 'n':
					so += '\n';
					break;
				case 'f':
					so += '\f';
					break;
				case 'u':
				case 'U':
					nchars = parseJsonUtf16(s, static_cast<int>(se - s), ucode);
					if (nchars < 0)
						return so.c_str(); //错误, 结束转义返回
					Unicode2Utf8(ucode, so);
					s += nchars - 2;
					break;
				}
				s += 2;
			}
			else {
				so += *s++;
			}
		}
		return so.c_str();
	}

	inline bool strneq(const char* s1, const char* s2, size_t  n)
	{
		if (!s1 || !s2)
			return false;
		size_t i = 0;
		for (; i < n; i++) {
			if (!s1[i] || !s2[i])
				break;
			if (s1[i] != s2[i])
				return false;
		}
		return i == n;
	}

	inline bool strnieq(const char* s1, const char* s2, size_t  n)
	{
		if (!s1 || !s2)
			return false;
		size_t i = 0;
		for (; i < n; i++) {
			if (!s1[i] || !s2[i])
				break;
			if (tolower(s1[i]) != tolower(s2[i]))
				return false;
		}
		return i == n;
	}

	template<typename _Str>
	void out_jstr(const char* s, size_t srcsize, _Str &sout)
	{ //escape and append  to sout,  escape  '\' -> '\\', '"' -> '\"' in s
		if (!s || !srcsize)
			return;
		if (!jstr_needesc(s, srcsize)) {
			sout.append(s, srcsize);
			return;
		}
		const char* se = s + srcsize;
		while (s < se) {
			outJsonEsc(*s++, sout);
		}
	}

	template<typename _Str>
	inline void from_jstr(const char* s, size_t srcsize, _Str &sout)
	{ // delete escape, "\\" -> '\', ""\'" -> '"' and set to sout
		jstr_fromesc(s, srcsize, sout);
	}

	/**
	 * @brief fixed length string
	 * @tparam chart : char or unsigned char
	*/
	template<typename chart = char,	class = std::enable_if<sizeof(chart) == 1>>
	class fixstring_
	{
	public:
		using value_type = chart;
		using pointer = chart*;
		using const_pointer = const chart*;
		using iterator = chart*;
		using const_iterator = const chart*;
		using reference = chart&;
		using const_reference = const chart&;
	private:
		size_t _size, _bufsize, _pos;
		pointer _buf;
	public:
		fixstring_(void* pbuf, size_t bufsize, size_t size = 0) : _size(size), _pos(0)
		{
			_bufsize = bufsize;
			_buf = (pointer)pbuf;
			if (size >= bufsize)
				_size = bufsize - 1;
		}
		~fixstring_() {
			endstr();
		}
		inline iterator begin() noexcept
		{
			return _buf;
		}
		inline const_iterator begin() const noexcept
		{
			return _buf;
		}
		inline iterator end() noexcept
		{
			return _buf + _size;
		}
		inline const_iterator end() const noexcept
		{
			return _buf + _size;
		}
		inline size_t size() const noexcept
		{
			return _size;
		}
		inline size_t length() const noexcept
		{
			return _size;
		}
		inline size_t max_size() const noexcept
		{
			return _bufsize - 1;
		}
		inline size_t capacity() const noexcept
		{
			return _bufsize - 1;
		}
		inline void clear() noexcept
		{
			_pos = 0;
			_size = 0;
			if (_buf && _bufsize)
				_buf[0] = 0;
		}
		inline bool empty() const noexcept
		{
			return 0 == _size;
		}
		inline void reserve(size_t n)
		{
		}
	public: //Element access
		inline reference operator[] (size_t pos)
		{
			return (pos < _bufsize) ? _buf[pos] : _buf[0];
		}
		inline const_reference operator[] (size_t pos) const
		{
			return (pos < _bufsize) ? _buf[pos] : _buf[0];
		}
		inline reference back() noexcept
		{
			if (!_size)
				return _buf[0];
			return _buf[_size - 1];
		}
		const_reference back() const noexcept
		{
			if (!_size)
				return _buf[0];
			return _buf[_size - 1];
		}
	public: //Modifiers
		fixstring_& append(const_pointer s, size_t n) noexcept
		{
			if (_size + (uint32_t)n < capacity()) {
				memcpy(_buf + _size, s, n);
				_size += (uint32_t)n;
			}
			return *this;
		}
		fixstring_& append(const char* s) noexcept
		{
			size_t zn = strlen(s);
			if (_size + (uint32_t)zn < capacity()) {
				memcpy(_buf + _size, s, zn);
				_size += (uint32_t)zn;
			}
			return *this;
		}
		fixstring_& assign(const value_type* p, size_t size) noexcept
		{
			clear();
			return append(p, size);
		}
		fixstring_& assign(const char* s) noexcept
		{
			clear();
			return append(s, strlen(s));
		}
		inline void push_back(value_type c) noexcept
		{
			if (_size + 1 < capacity()) {
				_buf[_size++] = c;
			}
		}
		inline fixstring_& operator= (const char* s) noexcept
		{
			clear();
			return append(s, strlen(s));
		}
		fixstring_& operator+= (value_type c) noexcept
		{
			if (_size + 1 < capacity()) {
				_buf[_size++] = c;
			}
			return *this;
		}
		inline fixstring_& operator+= (const char* s) noexcept
		{
			return append(s, strlen(s));
		}
	public: //operations
		void resize(size_t n) noexcept
		{
			if (n <= _bufsize)
				_size = (uint32_t)n;
		}
		inline const char* c_str() const noexcept
		{
			if (_size < _bufsize)
				_buf[_size] = 0;
			else {
				_buf[_bufsize - 1] = 0;
			}
			return (const char*)_buf;
		}
		inline char* c_str() noexcept
		{
			if (_size < _bufsize)
				_buf[_size] = 0;
			else {
				_buf[_bufsize - 1] = 0;
			}
			return (char*)_buf;
		}
		inline const_pointer data() const noexcept
		{
			return _buf;
		}
		inline pointer data() noexcept
		{
			return _buf;
		}
		inline void endstr() noexcept
		{
			if (!_buf || !_bufsize)
				return;
			if (_size < _bufsize)
				_buf[_size] = 0;
			else
				_buf[_bufsize - 1] = 0;
		}

#ifdef _WIN32
		bool format(const char* sfmt, ...) noexcept
#else
		bool format(const char* sfmt, ...) noexcept __attribute__((format(printf, 2, 3)))
#endif
		{
			if (!_buf || !_bufsize)
				return false;
			va_list arg_ptr;
			va_start(arg_ptr, sfmt);
			int n = vsnprintf(_buf, _bufsize, sfmt, arg_ptr);
			va_end(arg_ptr);
			if (n < 0 || n >= (int)_bufsize) {
				_size = 0;
				return false;
			}
			_size = n;
			return true;
		}
	public: // for stream
		static bool is_be() // is big endian
		{
			union {
				uint32_t u32;
				uint8_t u8;
			} ua;
			ua.u32 = 0x01020304;
			return ua.u8 == 0x01;
		}

		fixstring_& setpos(size_t pos) noexcept
		{
			_pos = pos > _size ? _size : (uint32_t)pos;
			return *this;
		};

		fixstring_& postoend() noexcept
		{
			_pos = _size;
			return *this;
		};

		inline size_t getpos() const
		{
			return _pos;
		};

		fixstring_& read(void* pbuf, size_t size) noexcept
		{
			if (_pos + size > _size)
				return *this; //throw std::out_of_range("out of range");
			memcpy(pbuf, (const uint8_t*)_buf + _pos, size);
			_pos += size;
			return *this;
		};

		fixstring_& write(const void* pbuf, size_t size) noexcept
		{
			if (_pos + size > _bufsize)
				return *this; // throw std::out_of_range("out of range");
			memcpy((uint8_t*)_buf + _pos, pbuf, size);
			_pos += size;
			if (_size < _pos)
				_size = _pos;
			return *this;
		};

		template < typename T, class = std::enable_if<std::is_arithmetic<T>::value>>
		fixstring_& operator >> (T& v) noexcept
		{ // read as little_endian
			if (_pos + sizeof(T) > _size)
				return *this;// throw std::out_of_range("out of range");
			if (!is_be())
				memcpy(&v, (const uint8_t*)_buf + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* ps = (uint8_t*)_buf + _pos, * pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T, class = std::enable_if<std::is_arithmetic<T>::value>>
		fixstring_& operator > (T& v) noexcept
		{  // read as big_endian
			if (_pos + sizeof(T) > _size)
				return *this; //throw std::out_of_range("out of range");
			if (is_be())
				memcpy(&v, (const uint8_t*)_buf + _pos, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* ps = (uint8_t*)_buf + _pos, * pd = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd-- = *ps++;
					n--;
				}
			}
			_pos += sizeof(T);
			return *this;
		}

		template < typename T, class = std::enable_if<std::is_arithmetic<T>::value>>
		fixstring_& operator << (T v) noexcept
		{  // write as little_endian
			if (_pos + sizeof(T) > _bufsize)
				return *this; //throw std::out_of_range("out of range");

			if (!is_be())
				memcpy((uint8_t*)_buf + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* pd = (uint8_t*)_buf + _pos, * ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			if (_size < _pos)
				_size = _pos;
			return *this;
		}

		template < typename T, class = std::enable_if<std::is_arithmetic<T>::value>>
		fixstring_& operator < (T v) noexcept
		{   // write as big_endian
			if (_pos + sizeof(T) > _bufsize)
				return *this; //throw std::out_of_range("out of range");
			if (is_be())
				memcpy((uint8_t*)_buf + _pos, &v, sizeof(T));
			else {
				int n = sizeof(T) - 1;
				uint8_t* pd = (uint8_t*)_buf + _pos, * ps = ((uint8_t*)&v) + n;
				while (n >= 0) {
					*pd++ = *ps--;
					n--;
				}
			}
			_pos += sizeof(T);
			if (_size < _pos)
				_size = _pos;
			return *this;
		}
	};

	using fixstring = fixstring_<char>;
	using chararray = fixstring_<char>;
	using bytearray = fixstring_<uint8_t>;
}// namespace ec

#define EC_STACKSTRING(varobj, bufsize)\
char s_##varobj[bufsize];\
ec::fixstring varobj(s_##varobj, bufsize, 0);

#define EC_DECLARE_CHARARRAY(varobj, bufsize)\
char s_##varobj[bufsize];\
ec::chararray varobj(s_##varobj, bufsize, 0);

#define EC_DECLARE_BYTEARRAY(varobj, bufsize)\
uint8_t s_##varobj[bufsize];\
ec::bytearray varobj(s_##varobj, bufsize, 0);

