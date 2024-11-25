/*!
\file ec_aes256.h

Modified from https://github.com/kokke/tiny-AES-c

轻量级的AES256编码和解码，用于配置文件加密，当没有openssl时使用。

\author	jiangyong
\email  kipway@outlook.com
\update
  2024.11.12 support no ec_alloctor

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <stdint.h>
#include <string.h>
#include "ec_base64.h"

#define aes_blocklen  16
#define aes_keylen  32
#define aes_keyexpsize  240
#define aes_Nb  4
#define aes_Nk  8
#define aes_Nr  14

#define getSBoxValue(num) (sbox[(num)])
#define getSBoxInvert(num) (rsbox[(num)])

namespace ec {
	class aes256
	{
	public:
		typedef uint8_t state_t[4][4];
		struct AES_ctx
		{
			uint8_t RoundKey[aes_keyexpsize];
			uint8_t Iv[aes_blocklen];
		};
	protected:
		AES_ctx _ctx;
	public:
		inline void init_key_iv(const uint8_t* key, const uint8_t* iv) // key 32bytes, iv 16bytes
		{
			KeyExpansion(_ctx.RoundKey, key);
			memcpy(_ctx.Iv, iv, aes_blocklen);
		}

		template<class _clsout>// _clsout=ec::bytes
		bool cbc_encode(const void* src, size_t srclen, _clsout& vout)
		{
			vout.clear();
			vout.append(src, srclen);
			int nlen = (int)srclen + 1;
			if (nlen % aes_blocklen) {
				int i;
				for (i = 0; i < (int)(aes_blocklen - (nlen % aes_blocklen)) + 1; i++)//padding and padding_length
					vout.push_back((uint8_t)(aes_blocklen - (nlen % aes_blocklen)));
			}
			else
				vout.push_back((uint8_t)0); //padding_length
			AES_CBC_encrypt_buffer((uint8_t*)vout.data(), (uint32_t)vout.size());
			return true;
		}

		template<class _clsout> // _clsout=ec::bytes
		bool cbc_decode(const void* src, size_t srclen, _clsout& vout)
		{
			vout.clear();
			vout.append(src, srclen);
			AES_CBC_decrypt_buffer((uint8_t*)vout.data(), (uint32_t)srclen);

			int ndel = vout.back();
			if (ndel < 0 || ndel >= aes_blocklen || ndel >= (int)srclen)
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
			vout.reserve(modp_b64_encode_len(bin.size()));
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
	private:
		void AES_CBC_encrypt_buffer(uint8_t* buf, uint32_t length)
		{
			uintptr_t i;
			AES_ctx *ctx = &_ctx;
			uint8_t *Iv = ctx->Iv;
			for (i = 0; i < length; i += aes_blocklen)
			{
				XorWithIv(buf, Iv);
				Cipher((state_t*)buf, ctx->RoundKey);
				Iv = buf;
				buf += aes_blocklen;
			}
			/* store Iv in ctx for next call */
			memcpy(ctx->Iv, Iv, aes_blocklen);
		}

		void AES_CBC_decrypt_buffer(uint8_t* buf, uint32_t length)
		{
			uintptr_t i;
			uint8_t storeNextIv[aes_blocklen];
			for (i = 0; i < length; i += aes_blocklen) {
				memcpy(storeNextIv, buf, aes_blocklen);
				InvCipher((state_t*)buf, _ctx.RoundKey);
				XorWithIv(buf, _ctx.Iv);
				memcpy(_ctx.Iv, storeNextIv, aes_blocklen);
				buf += aes_blocklen;
			}
		}

	private:
		// The lookup-tables are marked const so they can be placed in read-only storage instead of RAM
		// The numbers below can be computed dynamically trading ROM for RAM -
		// This can be useful in (embedded) bootloader applications, where ROM is often limited.
		const uint8_t sbox[256] = {
			//0     1    2      3     4    5     6     7      8    9     A      B    C     D     E     F
			0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
			0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
			0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
			0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
			0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
			0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
			0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
			0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
			0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
			0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
			0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
			0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
			0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
			0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
			0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
			0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16 };

		const uint8_t rsbox[256] = {
		  0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
		  0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
		  0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
		  0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
		  0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
		  0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
		  0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
		  0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
		  0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
		  0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
		  0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
		  0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
		  0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
		  0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
		  0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
		  0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d };

		// The round constant word array, Rcon[i], contains the values given by
		// x to the power (i-1) being powers of x (x is denoted as {02}) in the field GF(2^8)
		const uint8_t Rcon[11] = {
		  0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };

		// This function produces Nb(Nr+1) round keys. The round keys are used in each round to decrypt the states.
		void KeyExpansion(uint8_t* RoundKey, const uint8_t* Key)
		{
			unsigned i, j, k;
			uint8_t tempa[4]; // Used for the column/row operations

			// The first round key is the key itself.
			for (i = 0; i < aes_Nk; ++i) {
				RoundKey[(i * 4) + 0] = Key[(i * 4) + 0];
				RoundKey[(i * 4) + 1] = Key[(i * 4) + 1];
				RoundKey[(i * 4) + 2] = Key[(i * 4) + 2];
				RoundKey[(i * 4) + 3] = Key[(i * 4) + 3];
			}

			// All other round keys are found from the previous round keys.
			for (i = aes_Nk; i < aes_Nb * (aes_Nr + 1); ++i) {
				k = (i - 1) * 4;
				tempa[0] = RoundKey[k + 0];
				tempa[1] = RoundKey[k + 1];
				tempa[2] = RoundKey[k + 2];
				tempa[3] = RoundKey[k + 3];

				if (i % aes_Nk == 0) {
					// [a0,a1,a2,a3] becomes [a1,a2,a3,a0]
					const uint8_t u8tmp = tempa[0];
					tempa[0] = tempa[1];
					tempa[1] = tempa[2];
					tempa[2] = tempa[3];
					tempa[3] = u8tmp;

					// applies the S-box to each of the four bytes to produce an output word.
					tempa[0] = getSBoxValue(tempa[0]);
					tempa[1] = getSBoxValue(tempa[1]);
					tempa[2] = getSBoxValue(tempa[2]);
					tempa[3] = getSBoxValue(tempa[3]);

					tempa[0] = tempa[0] ^ Rcon[i / aes_Nk];
				}

				if (i % aes_Nk == 4) {
					tempa[0] = getSBoxValue(tempa[0]);
					tempa[1] = getSBoxValue(tempa[1]);
					tempa[2] = getSBoxValue(tempa[2]);
					tempa[3] = getSBoxValue(tempa[3]);
				}

				j = i * 4; k = (i - aes_Nk) * 4;
				RoundKey[j + 0] = RoundKey[k + 0] ^ tempa[0];
				RoundKey[j + 1] = RoundKey[k + 1] ^ tempa[1];
				RoundKey[j + 2] = RoundKey[k + 2] ^ tempa[2];
				RoundKey[j + 3] = RoundKey[k + 3] ^ tempa[3];
			}
		}

		void XorWithIv(uint8_t* buf, const uint8_t* Iv)
		{
			uint8_t i;
			for (i = 0; i < aes_blocklen; ++i) // The block in AES is always 128bit no matter the key size
				buf[i] ^= Iv[i];
		}

		// This function adds the round key to state.
		// The round key is added to the state by an XOR function.
		void AddRoundKey(uint8_t round, state_t* state, const uint8_t* RoundKey)
		{
			uint8_t i, j;
			for (i = 0; i < 4; ++i) {
				for (j = 0; j < 4; ++j)
					(*state)[i][j] ^= RoundKey[(round * aes_Nb * 4) + (i * aes_Nb) + j];
			}
		}

		// The SubBytes Function Substitutes the values in the
		// state matrix with values in an S-box.
		void SubBytes(state_t* state)
		{
			uint8_t i, j;
			for (i = 0; i < 4; ++i) {
				for (j = 0; j < 4; ++j)
					(*state)[j][i] = getSBoxValue((*state)[j][i]);
			}
		}

		// The ShiftRows() function shifts the rows in the state to the left.
		// Each row is shifted with different offset.
		// Offset = Row number. So the first row is not shifted.
		void ShiftRows(state_t* state)
		{
			uint8_t temp;

			// Rotate first row 1 columns to left
			temp = (*state)[0][1];
			(*state)[0][1] = (*state)[1][1];
			(*state)[1][1] = (*state)[2][1];
			(*state)[2][1] = (*state)[3][1];
			(*state)[3][1] = temp;

			// Rotate second row 2 columns to left
			temp = (*state)[0][2];
			(*state)[0][2] = (*state)[2][2];
			(*state)[2][2] = temp;

			temp = (*state)[1][2];
			(*state)[1][2] = (*state)[3][2];
			(*state)[3][2] = temp;

			// Rotate third row 3 columns to left
			temp = (*state)[0][3];
			(*state)[0][3] = (*state)[3][3];
			(*state)[3][3] = (*state)[2][3];
			(*state)[2][3] = (*state)[1][3];
			(*state)[1][3] = temp;
		}

		inline uint8_t xtime(uint8_t x)
		{
			return ((x << 1) ^ (((x >> 7) & 1) * 0x1b));
		}

		// MixColumns function mixes the columns of the state matrix
		void MixColumns(state_t* state)
		{
			uint8_t i;
			uint8_t Tmp, Tm, t;
			for (i = 0; i < 4; ++i) {
				t = (*state)[i][0];
				Tmp = (*state)[i][0] ^ (*state)[i][1] ^ (*state)[i][2] ^ (*state)[i][3];
				Tm = (*state)[i][0] ^ (*state)[i][1]; Tm = xtime(Tm);  (*state)[i][0] ^= Tm ^ Tmp;
				Tm = (*state)[i][1] ^ (*state)[i][2]; Tm = xtime(Tm);  (*state)[i][1] ^= Tm ^ Tmp;
				Tm = (*state)[i][2] ^ (*state)[i][3]; Tm = xtime(Tm);  (*state)[i][2] ^= Tm ^ Tmp;
				Tm = (*state)[i][3] ^ t;              Tm = xtime(Tm);  (*state)[i][3] ^= Tm ^ Tmp;
			}
		}

		void Cipher(state_t* state, const uint8_t* RoundKey)
		{
			uint8_t round = 0;

			// Add the First round key to the state before starting the rounds.
			AddRoundKey(0, state, RoundKey);

			// There will be Nr rounds.
			// The first Nr-1 rounds are identical.
			// These Nr rounds are executed in the loop below.
			// Last one without MixColumns()
			for (round = 1; ; ++round) {
				SubBytes(state);
				ShiftRows(state);
				if (round == aes_Nr)
					break;
				MixColumns(state);
				AddRoundKey(round, state, RoundKey);
			}
			// Add round key to last round
			AddRoundKey(aes_Nr, state, RoundKey);
		}

		void InvShiftRows(state_t* state)
		{
			uint8_t temp;

			// Rotate first row 1 columns to right
			temp = (*state)[3][1];
			(*state)[3][1] = (*state)[2][1];
			(*state)[2][1] = (*state)[1][1];
			(*state)[1][1] = (*state)[0][1];
			(*state)[0][1] = temp;

			// Rotate second row 2 columns to right
			temp = (*state)[0][2];
			(*state)[0][2] = (*state)[2][2];
			(*state)[2][2] = temp;

			temp = (*state)[1][2];
			(*state)[1][2] = (*state)[3][2];
			(*state)[3][2] = temp;

			// Rotate third row 3 columns to right
			temp = (*state)[0][3];
			(*state)[0][3] = (*state)[1][3];
			(*state)[1][3] = (*state)[2][3];
			(*state)[2][3] = (*state)[3][3];
			(*state)[3][3] = temp;
		}

		// The SubBytes Function Substitutes the values in the
		// state matrix with values in an S-box.
		void InvSubBytes(state_t* state)
		{
			uint8_t i, j;
			for (i = 0; i < 4; ++i) {
				for (j = 0; j < 4; ++j)
					(*state)[j][i] = rsbox[(*state)[j][i]];
			}
		}

		inline uint8_t Multiply(uint8_t x, uint8_t y)
		{
			return (((y & 1) * x) ^
				((y >> 1 & 1) * xtime(x)) ^
				((y >> 2 & 1) * xtime(xtime(x))) ^
				((y >> 3 & 1) * xtime(xtime(xtime(x)))) ^
				((y >> 4 & 1) * xtime(xtime(xtime(xtime(x))))));
		}

		// MixColumns function mixes the columns of the state matrix.
		// The method used to multiply may be difficult to understand for the inexperienced.
		// Please use the references to gain more information.
		void InvMixColumns(state_t* state)
		{
			int i;
			uint8_t a, b, c, d;
			for (i = 0; i < 4; ++i) {
				a = (*state)[i][0];
				b = (*state)[i][1];
				c = (*state)[i][2];
				d = (*state)[i][3];

				(*state)[i][0] = Multiply(a, 0x0e) ^ Multiply(b, 0x0b) ^ Multiply(c, 0x0d) ^ Multiply(d, 0x09);
				(*state)[i][1] = Multiply(a, 0x09) ^ Multiply(b, 0x0e) ^ Multiply(c, 0x0b) ^ Multiply(d, 0x0d);
				(*state)[i][2] = Multiply(a, 0x0d) ^ Multiply(b, 0x09) ^ Multiply(c, 0x0e) ^ Multiply(d, 0x0b);
				(*state)[i][3] = Multiply(a, 0x0b) ^ Multiply(b, 0x0d) ^ Multiply(c, 0x09) ^ Multiply(d, 0x0e);
			}
		}

		void InvCipher(state_t* state, const uint8_t* RoundKey)
		{
			uint8_t round = 0;

			// Add the First round key to the state before starting the rounds.
			AddRoundKey(aes_Nr, state, RoundKey);

			// There will be Nr rounds.
			// The first Nr-1 rounds are identical.
			// These Nr rounds are executed in the loop below.
			// Last one without InvMixColumn()
			for (round = (aes_Nr - 1); ; --round) {
				InvShiftRows(state);
				InvSubBytes(state);
				AddRoundKey(round, state, RoundKey);
				if (round == 0)
					break;
				InvMixColumns(state);
			}
		}
	}; // aes256	
}// namespace ec