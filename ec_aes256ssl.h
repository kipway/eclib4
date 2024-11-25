/*!
\file ec_aes256sll.h

使用openssl实现的AES加密解密，适合有openssl库时使用。

\author	jiangyong
\email  kipway@outlook.com
\update 2024.5.13

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <stdint.h>
#include <string.h>
#include "ec_string.hpp"
#include "ec_base64.h"
#include "openssl/aes.h"
namespace ec
{
	class aes256ssl
	{
	private:
		uint8_t _key[32];
		uint8_t _iv[16];
	public:
		inline void init_key_iv(const uint8_t* key, const uint8_t* iv) // key 32bytes, iv 16bytes
		{
			memcpy(_key, key, 32);
			memcpy(_iv, iv, 16);
		}

		template<class _clsout>// _clsout=ec::bytes
		bool cbc_encode(const void* src, size_t srclen, _clsout& vout)
		{
			AES_KEY aes_e;
			unsigned char iv[16];
			memcpy(iv, _iv, 16);
			if (AES_set_encrypt_key(_key, 256, &aes_e) < 0)
				return false;
			_clsout vtmp;
			vtmp.append((uint8_t*)src, srclen);
			int nlen = (int)srclen + 1;
			if (nlen % AES_BLOCK_SIZE) {
				int i;
				for (i = 0; i < (int)(AES_BLOCK_SIZE - (nlen % AES_BLOCK_SIZE)) + 1; i++)//padding and padding_length
					vtmp.push_back((char)(AES_BLOCK_SIZE - (nlen % AES_BLOCK_SIZE)));
			}
			else
				vtmp.push_back(0); //padding_length

			vout.clear();
			vout.resize(vtmp.size());
			AES_cbc_encrypt((const unsigned char*)vtmp.data(), (unsigned char*)vout.data(), vtmp.size(), &aes_e, iv, AES_ENCRYPT);
			return true;
		}

		template<class _clsout> // _clsout=ec::bytes
		bool cbc_decode(const void* src, size_t srclen, _clsout& vout)
		{
			AES_KEY aes_d;
			unsigned char iv[16];
			memcpy(iv, _iv, 16);
			if (AES_set_decrypt_key(_key, 256, &aes_d) < 0)
				return false;
			vout.clear();
			vout.ressize(srclen);
			AES_cbc_encrypt((const unsigned char*)src, (unsigned char*)vout.data(), srclen, &aes_d, iv, AES_DECRYPT);
			int ndel = vout.back();
			if (ndel < 0 || ndel > 15 || ndel >= (int)srclen)
				return false;
			vout.resize(srclen - ndel - 1);
			return true;
		}

		// first aes256cbc encode , then base64 encode	
		template<class _Out>//ec::string
		static bool aes256cbc_base64_encode(const uint8_t* key, const uint8_t* iv,
			const void* s, size_t size, _Out& vout)
		{
			if (!s || !size)
				return true;
			aes256 aes;
			aes.init_key_iv(key, iv);
			ec::bytes bin;
			if (!aes.cbc_encode(s, size, bin))
				return false;

			vout.clear();
			vout.resize(modp_b64_encode_len(bin.size()));
			int n = ec::encode_base64(vout.data(), (char*)bin.data(), (int)bin.size());
			if (n <= 0)
				return false;
			vout.resize(n);
			return true;
		}

		// first decode base64 , then decode aes256cbc
		template<class _Out>//ec::string
		static bool aes256cbc_base64_decode(const uint8_t* key, const uint8_t* iv,
			const char* s, size_t size, _Out& vout)
		{
			if (!s || !size)
				return true;
			aes256 aes;
			aes.init_key_iv(key, iv);
			ec::autobuf<> obase(modp_b64_decode_len(size));
			int n = ec::decode_base64(obase.data(), s, (int)size);
			if (n <= 0)
				return false;
			return aes.cbc_decode(obase.data(), n, vout);
		}
	};//class aes256ssl
}// namespace ec
