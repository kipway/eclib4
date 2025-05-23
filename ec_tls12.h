﻿/*!
\file ec_tls12.h
\author	jiangyong
\email  kipway@outlook.com
\update:
  2024.11.25 replace sessionclient::_pubkey from array to ec::bytes
  2024.11.9  support no ec_alloctor
  2024.2.1   clear include
  2023.11.4  support root_chain pem format
  2023.7.4   Fix mkr_ClientKeyExchange
  2023.6.26  remove ec:array, fix mkr_ClientHelloMsg compression_methods
  2023.5.13  remove ec::memory
  2023.2.6   add srvca::isok()

TLS1.2(rfc5246)  session class

support:
CipherSuite TLS_RSA_WITH_AES_128_CBC_SHA256 = { 0x00,0x3C };
CipherSuite TLS_RSA_WITH_AES_256_CBC_SHA256 = { 0x00,0x3D };

CipherSuite TLS_RSA_WITH_AES_128_CBC_SHA = {0x00,0x2F};
CipherSuite TLS_RSA_WITH_AES_256_CBC_SHA = {0x00,0x35};

tls_session
	session base class

tls_session_cli
	session class for client

tls_session_srv
	session class for server

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once

#define  tls_rec_fragment_len 16384

#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "ec_string.h"
#include "ec_log.h"
#include "ec_string.hpp"
#include "ec_stream.h"
#include "ec_vstream.hpp"

#ifdef _WIN32
#ifdef _OPENSSL_1_1_X
#pragma comment(lib,"libcrypto.lib")
#else
#pragma comment(lib,"libeay32.lib")
#endif
#endif

#include "openssl/rand.h"
#include "openssl/x509.h"
#include "openssl/hmac.h"
#include "openssl/aes.h"
#include "openssl/pem.h"

/*!
\brief CipherSuite
*/
#define TLS_RSA_WITH_AES_128_CBC_SHA    0x2F
#define TLS_RSA_WITH_AES_256_CBC_SHA    0x35
#define TLS_RSA_WITH_AES_128_CBC_SHA256 0x3C
#define TLS_RSA_WITH_AES_256_CBC_SHA256 0x3D
#define TLS_COMPRESS_NONE   0

#define TLSVER_MAJOR        3
#define TLSVER_NINOR        3

#define TLS_CBCBLKSIZE  16292   // (16384-16-32-32 - 8)

#define TLS_SESSION_ERR		(-1)// error
#define TLS_SESSION_NONE    0
#define TLS_SESSION_OK		1   // need send data
#define TLS_SESSION_HKOK	2   // handshack ok
#define TLS_SESSION_APPDATA 3   // on app data

#define TLS_REC_BUF_SIZE (1024 * 18)

#ifndef _OPENSSL_1_1_X
#define X509_get0_pubkey_bitstr(_x509) (_x509)->cert_info->key->public_key
#endif
namespace ec
{
	namespace tls {
		enum rec_contenttype {
			rec_change_cipher_spec = 20,
			rec_alert = 21,
			rec_handshake = 22,
			rec_application_data = 23,
			rec_max = 255
		};
		enum handshaketype {
			hsk_hello_request = 0,
			hsk_client_hello = 1,
			hsk_server_hello = 2,
			hsk_certificate = 11,
			hsk_server_key_exchange = 12,
			hsk_certificate_request = 13,
			hsk_server_hello_done = 14,
			hsk_certificate_verify = 15,
			hsk_client_key_exchange = 16,
			hsk_finished = 20,
			hsk_max = 255
		};
	}

	template<class _strOut>
	bool load_certfile(const char* filecert, _strOut& out)
	{
		out.clear();
		unsigned char stmp[4096];
		FILE* pf = fopen(filecert, "rb");
		if (!pf)
			return false;
		size_t size;
		using ctype = typename _strOut::value_type;
		while (!feof(pf)) {
			size = fread(stmp, 1, sizeof(stmp), pf);
			out.append((const ctype*)stmp, size);
		}
		fclose(pf);
		return out.size() > 5u;
	}

	template<class _strOut>
	bool x509toDer(X509* px509, _strOut& out)
	{
		int len;
		unsigned char* buf, * pdobuf;
		len = i2d_X509(px509, nullptr);
		if (len < 0)
			return false;
		buf = (unsigned char*)ec::g_malloc(len + 1);
		pdobuf = buf;
		if (i2d_X509(px509, &pdobuf) < 0) {
			ec::g_free(buf);
			return false;
		}
		out.clear();
		using ctype = typename _strOut::value_type;
		out.assign((const ctype*)buf, len);
		ec::g_free(buf);
		return true;
	}

	template<class _Out>
	bool get_cert_pkey(const char* filecert, _Out* pout)//get ca public key bitstr
	{
		ec::string cert;
		if (!load_certfile(filecert, cert))
			return false;
		X509* px509 = nullptr;
		if (strstr(cert.c_str(), "-----BEGIN CERTIFICATE-----")) {
			BIO* bioPub = BIO_new_mem_buf(cert.data(), (int)cert.size());
			px509 = PEM_read_bio_X509(bioPub, nullptr, nullptr, nullptr);
			BIO_free(bioPub);
		}
		else { // der
			const unsigned char* p = (const unsigned char*)cert.data();
			px509 = d2i_X509(nullptr, &p, (long)cert.size());
		}
		if (!px509)
			return false;
		pout->clear();
		using ctype = typename _Out::value_type;
		ASN1_BIT_STRING* pbitstr = X509_get0_pubkey_bitstr(px509);
		pout->assign((const ctype*)pbitstr->data, (size_t)pbitstr->length);
		X509_free(px509);
		return true;
	}

	class tls_srvca
	{
	private:
		RSA* _pRsaPub;
		RSA* _pRsaPrivate;

		ec::string _pcer;
		ec::string _prootcer;

		std::mutex _csRsa;
	public:
		tls_srvca() : _pRsaPub(nullptr), _pRsaPrivate(nullptr)
		{
			_pcer.reserve(4096);
			_prootcer.reserve(8192);
		}
		inline bool isok() {
			return _pRsaPub && _pRsaPrivate;
		}
		void clear() {
			if (_pRsaPrivate)
				RSA_free(_pRsaPrivate);
			if (_pRsaPub)
				RSA_free(_pRsaPub);
			_pRsaPub = nullptr;
			_pRsaPrivate = nullptr;
		}
		~tls_srvca()
		{
			clear();
		}
		bool InitCert(const char* filecert, const char* filerootcert, const char* fileprivatekey)
		{
			std::unique_lock<std::mutex> lck(_csRsa);
			clear();
			if (!load_certfile(filecert, _pcer))
				return false;
			if (filerootcert && *filerootcert) {
				if (!load_certfile(filerootcert, _prootcer))
					return false;
				if (strstr(_prootcer.c_str(), "-----BEGIN CERTIFICATE-----")) {
					BIO* bioPub = BIO_new_mem_buf(_prootcer.data(), (int)_prootcer.size());
					X509* px509 = PEM_read_bio_X509(bioPub, nullptr, nullptr, nullptr);
					BIO_free(bioPub);
					if (!px509)
						return false;
					if (!x509toDer(px509, _prootcer)) {
						X509_free(px509);
						return false;;
					}
					X509_free(px509);
				}
			}

			FILE* pf = fopen(fileprivatekey, "rb");
			if (!pf)
				return false;
			_pRsaPrivate = PEM_read_RSAPrivateKey(pf, nullptr, nullptr, nullptr);
			fclose(pf);
			if (!_pRsaPrivate) {
				return false;
			}
			bool bresult = false;
			EVP_PKEY* pevppk = nullptr;
			X509* px509 = nullptr;
			do {
				if (strstr(_pcer.c_str(), "-----BEGIN CERTIFICATE-----")) {
					BIO* bioPub = BIO_new_mem_buf(_pcer.data(), (int)_pcer.size());
					px509 = PEM_read_bio_X509(bioPub, nullptr, nullptr, nullptr);
					BIO_free(bioPub);
					if (!px509)
						break;
					if (!x509toDer(px509, _pcer))
						break;
				}
				else { // der
					const unsigned char* p = (const unsigned char*)_pcer.data();
					px509 = d2i_X509(nullptr, &p, (long)_pcer.size());
					if (!px509)
						break;;
				}
				pevppk = X509_get_pubkey(px509);
				if (!pevppk)
					break;
				_pRsaPub = EVP_PKEY_get1_RSA(pevppk); //get copy of RSA public key
				if (!_pRsaPub)
					break;
				bresult = true;
			} while (0);

			if (pevppk)
				EVP_PKEY_free(pevppk);
			if (px509)
				X509_free(px509);
			if (!bresult)
				clear();
			return bresult;
		}
		/**
		 * @brief 重新设置证书
		 * @param filecert 证书
		 * @param filerootcert 根证书
		 * @param fileprivatekey 私钥
		 * @return -1:读取新证书错误; 0:没有改变； 1：成功替换
		 */
		int ReSetCert(const char* filecert, const char* filerootcert, const char* fileprivatekey)
		{
			tls_srvca tmp;
			if (!tmp.InitCert(filecert, filerootcert, fileprivatekey))
				return -1;
			if (isSameCert(tmp._pcer) && isSameRootCert(tmp._prootcer))
				return 0;
			moveFrom(tmp);
			return 1;
		}

		/**
		 * @brief 私钥解密
		 * @param flen 长度
		 * @param psrc 密文
		 * @param poubuf 输出明文
		 * @return 解密后的字节数
		 */
		int PrivateDecrypt(int flen, const unsigned char* psrc, unsigned char* poubuf)
		{
			int nbytes;
			_csRsa.lock();
			nbytes = RSA_private_decrypt(flen, psrc, poubuf, _pRsaPrivate, RSA_PKCS1_PADDING);
			_csRsa.unlock();
			return nbytes;
		}

		bool isSameCert(ec::string& ca)
		{
			std::unique_lock<std::mutex> lck(_csRsa);
			if (_pcer.size() != ca.size())
				return false;
			const char* s1 = _pcer.data();
			const char* s2 = ca.data();
			size_t i, z = _pcer.size();
			for (i = 0; i < z; i++) {
				if (*s1 != *s2)
					return false;
				++s1;
				++s2;
			}
			return true;
		}

		bool isSameRootCert(ec::string& caroot)
		{
			std::unique_lock<std::mutex> lck(_csRsa);
			if (_prootcer.size() != caroot.size())
				return false;
			const char* s1 = _prootcer.data();
			const char* s2 = caroot.data();
			size_t i, z = _prootcer.size();
			for (i = 0; i < z; i++) {
				if (*s1 != *s2)
					return false;
				++s1;
				++s2;
			}
			return true;
		}

		void moveFrom(tls_srvca& ca) //移动，用于在线替换新证书
		{
			std::unique_lock<std::mutex> lck(_csRsa);
			clear();
			_pcer.swap(ca._pcer);
			_prootcer.swap(ca._prootcer);
			_pRsaPub = ca._pRsaPub;
			_pRsaPrivate = ca._pRsaPrivate;
			ca._pRsaPub = nullptr;
			ca._pRsaPrivate = nullptr;
		}

		inline bool empty() const
		{
			return nullptr == _pRsaPub;
		}

		bool MakeCertificateMsg(ec::vstream& vo)
		{
			std::unique_lock<std::mutex> lck(_csRsa);
			vo.clear();
			vo.push_back((uint8_t)tls::hsk_certificate);
			vo.push_back((uint8_t)0);
			vo.push_back((uint8_t)0);
			vo.push_back((uint8_t)0);//1,2,3
			uint32_t u;
			if (!_prootcer.empty()) {
				u = (uint32_t)(_pcer.size() + _prootcer.size() + 6);
				vo.push_back((uint8_t)((u >> 16) & 0xFF));
				vo.push_back((uint8_t)((u >> 8) & 0xFF));
				vo.push_back((uint8_t)(u & 0xFF));//4,5,6

				u = (uint32_t)_pcer.size();
				vo.push_back((uint8_t)((u >> 16) & 0xFF));
				vo.push_back((uint8_t)((u >> 8) & 0xFF));
				vo.push_back((uint8_t)(u & 0xFF));//7,8,9
				vo.append((const uint8_t*)_pcer.data(), _pcer.size());

				u = (uint32_t)_prootcer.size();
				vo.push_back((uint8_t)((u >> 16) & 0xFF));
				vo.push_back((uint8_t)((u >> 8) & 0xFF));
				vo.push_back((uint8_t)(u & 0xFF));
				vo.append((const uint8_t*)_prootcer.data(), _prootcer.size());
			}
			else {
				u = (uint32_t)_pcer.size() + 3;
				vo.push_back((uint8_t)((u >> 16) & 0xFF));
				vo.push_back((uint8_t)((u >> 8) & 0xFF));
				vo.push_back((uint8_t)(u & 0xFF));//4,5,6

				u = (uint32_t)_pcer.size();
				vo.push_back((uint8_t)((u >> 16) & 0xFF));
				vo.push_back((uint8_t)((u >> 8) & 0xFF));
				vo.push_back((uint8_t)(u & 0xFF));//7,8,9
				vo.append((const uint8_t*)_pcer.data(), _pcer.size());
			}
			u = (uint32_t)vo.size() - 4;
			*(vo.data() + 1) = (uint8_t)((u >> 16) & 0xFF);
			*(vo.data() + 2) = (uint8_t)((u >> 8) & 0xFF);
			*(vo.data() + 3) = (uint8_t)((u >> 0) & 0xFF);
			return true;
		}
	};

	namespace tls {
		class handshake // Handshake messages
		{
		public:
			handshake()
			{
				_srv_certificate.reserve(4000);
			}
		public:
			ec::vstream _srv_hello, _srv_certificate, _srv_hellodone;
			ec::vstream _cli_hello, _cli_key_exchange, _cli_finished;
		public:
			_USE_EC_OBJ_ALLOCATOR
			template<class _Out>
			void out(_Out* p, bool bfin = false)
			{
				p->clear();
				p->append(_cli_hello.data(), _cli_hello.size());
				p->append(_srv_hello.data(), _srv_hello.size());
				p->append(_srv_certificate.data(), _srv_certificate.size());
				p->append(_srv_hellodone.data(), _srv_hellodone.size());
				p->append(_cli_key_exchange.data(), _cli_key_exchange.size());
				if (bfin)
					p->append(_cli_finished.data(), _cli_finished.size());
			}
			void clear()
			{
				_srv_hello.clear();
				_srv_certificate.clear();
				_srv_hellodone.clear();
				_cli_hello.clear();
				_cli_key_exchange.clear();
				_cli_finished.clear();
			}
		};

		/*!
		\brief base class for TLS 1.2 session
		*/
		class session
		{
		public:
			session(const session&) = delete;
			session& operator = (const session&) = delete;
			session(bool bserver, unsigned int ucid, ilog* plog) : _plog(plog),
				_ucid(ucid), _bserver(bserver), _breadcipher(false), _bsendcipher(false), _seqno_send(0), _seqno_read(0), _cipher_suite(0),
				_bhandshake_finished(false)
			{
				_key_swmac[0] = 0;
				_keyblock[0] = 0;
				_serverrand[0] = 0;
				resetblks();
				_hmsg = new handshake;
			};
			
			session(session*p) : _plog(p->_plog), _ucid(p->_ucid), _bserver(p->_bserver), _breadcipher(p->_breadcipher),
				_bsendcipher(p->_bsendcipher), _seqno_send(p->_seqno_send), _seqno_read(p->_seqno_read), _cipher_suite(p->_cipher_suite),
				_bhandshake_finished(p->_bhandshake_finished)
			{
				_pkgtcp.free();
				_pkgtcp.append(p->_pkgtcp.data_(), p->_pkgtcp.size_());

				memcpy(_keyblock, p->_keyblock, sizeof(_keyblock));
				memcpy(_key_cwmac, p->_key_cwmac, sizeof(_key_cwmac));
				memcpy(_key_swmac, p->_key_swmac, sizeof(_key_swmac));
				memcpy(_key_cw, p->_key_cw, sizeof(_key_cw));
				memcpy(_key_sw, p->_key_sw, sizeof(_key_sw));

				_hmsg = p->_hmsg;
				p->_hmsg = nullptr; //move

				memcpy(_serverrand, p->_serverrand, sizeof(_serverrand));
				memcpy(_clientrand, p->_clientrand, sizeof(_clientrand));
				memcpy(_master_key, p->_master_key, sizeof(_master_key));
				memcpy(_key_block, p->_key_block, sizeof(_key_block));
			}

			session(session&& v) : _plog(v._plog), _ucid(v._ucid), _bserver(v._bserver), _breadcipher(v._breadcipher),
				_bsendcipher(v._bsendcipher), _seqno_send(v._seqno_send), _seqno_read(v._seqno_read), _cipher_suite(v._cipher_suite),
				_pkgtcp(std::move(v._pkgtcp)), _bhandshake_finished(v._bhandshake_finished)
			{
				memcpy(_keyblock, v._keyblock, sizeof(_keyblock));
				memcpy(_key_cwmac, v._key_cwmac, sizeof(_key_cwmac));
				memcpy(_key_swmac, v._key_swmac, sizeof(_key_swmac));
				memcpy(_key_cw, v._key_cw, sizeof(_key_cw));
				memcpy(_key_sw, v._key_sw, sizeof(_key_sw));

				_hmsg = v._hmsg;
				v._hmsg = nullptr; //move

				memcpy(_serverrand, v._serverrand, sizeof(_serverrand));
				memcpy(_clientrand, v._clientrand, sizeof(_clientrand));
				memcpy(_master_key, v._master_key, sizeof(_master_key));
				memcpy(_key_block, v._key_block, sizeof(_key_block));
			}

			virtual ~session()
			{
				if (_hmsg) {
					delete _hmsg;
					_hmsg = nullptr;
				}
			};
			inline uint32_t get_ucid()
			{
				return _ucid;
			}
			inline void appendreadbytes(const void* pdata, size_t size) {
				_pkgtcp.append(pdata, size);
			}
			inline uint16_t getCipherSuite()
			{
				return _cipher_suite;
			}
		protected:
			ilog* _plog;

			uint32_t _ucid;
			bool   _bserver, _breadcipher, _bsendcipher; // read/ write start use cipher

			uint64_t _seqno_send, _seqno_read;
			uint16_t _cipher_suite;

			parsebuffer _pkgtcp;

			uint8_t _keyblock[256], _key_cwmac[32], _key_swmac[32];// client_write_MAC_key,server_write_MAC_key
			uint8_t _key_cw[32], _key_sw[32];   // client_write_key,server_write_key

			handshake *_hmsg;
			uint8_t  _serverrand[32], _clientrand[32], _master_key[48], _key_block[256];
			bool  _bhandshake_finished;
			inline void resetblks()
			{
				memset(_keyblock, 0, sizeof(_keyblock));
				memset(_key_cwmac, 0, sizeof(_key_cwmac));
				memset(_key_swmac, 0, sizeof(_key_swmac));
				memset(_serverrand, 0, sizeof(_serverrand));
				memset(_clientrand, 0, sizeof(_clientrand));
				memset(_master_key, 0, sizeof(_master_key));
				memset(_key_block, 0, sizeof(_key_block));
			}
		private:
			//Calculate the hashmac value of the TLS record, speed optimized version,Reserve 13 bytes in front of pout
			bool caldatahmac(uint8_t type, uint64_t seqno, void* pd, size_t len, uint8_t* pkeymac, uint8_t *outmac)
			{
				uint8_t *ps = ((uint8_t*)pd) - 13;
				ec::stream es(ps, len + 13);
				es < seqno < type < (char)TLSVER_MAJOR < (char)TLSVER_NINOR < (unsigned short)len;
				unsigned int mdlen = 0;
				if (_cipher_suite == TLS_RSA_WITH_AES_128_CBC_SHA || _cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA)
					return HMAC(EVP_sha1(), pkeymac, 20, ps, len + 13, outmac, &mdlen) != NULL;
				return HMAC(EVP_sha256(), pkeymac, 32, ps, len + 13, outmac, &mdlen) != NULL;
			}

			bool decrypt_record(const uint8_t*pd, size_t len, uint8_t* pout, int *poutsize)// Reserve 8 bytes in front of pout
			{
				size_t maclen = 32;
				if (_cipher_suite == TLS_RSA_WITH_AES_128_CBC_SHA || _cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA)
					maclen = 20;
				if (len < 53) // 5 + pading16(IV + maclen + datasize)
					return false;

				unsigned char *sout = pout + 5, iv[AES_BLOCK_SIZE], *pkey = _key_sw, *pkmac = _key_swmac;
				AES_KEY aes_d;
				int nkeybit = 128;
				if (_cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA256 || _cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA)
					nkeybit = 256;

				if (_bserver) {
					pkey = _key_cw;
					pkmac = _key_cwmac;
				}
				memcpy(iv, pd + 5, AES_BLOCK_SIZE);//Decrypt
				if (AES_set_decrypt_key(pkey, nkeybit, &aes_d) < 0)
					return false;
				AES_cbc_encrypt((const unsigned char*)pd + 5 + AES_BLOCK_SIZE, (unsigned char*)sout, len - 5 - AES_BLOCK_SIZE, &aes_d, iv, AES_DECRYPT);

				unsigned int ufsize = sout[len - 5 - AES_BLOCK_SIZE - 1];//verify data MAC
				if (ufsize > 15)
					return false;

				size_t datasize = len - 5 - AES_BLOCK_SIZE - 1 - ufsize - maclen;
				if (datasize > tls_rec_fragment_len)
					return false;

				unsigned char mac[32], macsrv[32];
				memcpy(macsrv, &sout[datasize], maclen);
				if (!caldatahmac(pd[0], _seqno_read, sout, datasize, pkmac, mac))
					return false;
				if (memcmp(mac, macsrv, maclen))
					return false;

				*((uint32_t*)pout) = *((uint32_t*)pd);
				*(pout + 3) = ((datasize >> 8) & 0xFF);
				*(pout + 4) = (datasize & 0xFF);
				*poutsize = (int)datasize + 5;
				_seqno_read++;
				return true;
			}
		protected:
			template <class _Out>
			int MKR_WithAES_BLK(_Out *pout, uint8_t rectype, const uint8_t* sblk, size_t size)
			{
				int i;
				uint8_t* pkeyw = _bserver ? _key_sw : _key_cw, *pkeywmac = _bserver ? _key_swmac : _key_cwmac;
				uint8_t IV[AES_BLOCK_SIZE];//rand IV
				uint8_t mac[32], ssmem[TLS_REC_BUF_SIZE], esmem[TLS_REC_BUF_SIZE];
				ec::fixstring_<uint8_t> ss(ssmem, sizeof(ssmem)), es(esmem, sizeof(esmem));

				// calculate HMAC
				es < _seqno_send < rectype < (char)TLSVER_MAJOR < (char)TLSVER_NINOR < (unsigned short)size;
				es.write(sblk, size); //content
				unsigned int mdlen = 0;
				unsigned char *hmacret = nullptr;
				if (_cipher_suite == TLS_RSA_WITH_AES_128_CBC_SHA || _cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA)
					hmacret = HMAC(EVP_sha1(), pkeywmac, 20, es.data(), es.size(), mac, &mdlen);
				else
					hmacret = HMAC(EVP_sha256(), pkeywmac, 32, es.data(), es.size(), mac, &mdlen);
				if (!hmacret)
					return -1;

				// encrypt
				es.write(mac, (_cipher_suite == TLS_RSA_WITH_AES_128_CBC_SHA || _cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA) ? 20 : 32); //MAC
				size_t len = es.getpos() + 1 - 13;
				if (len % AES_BLOCK_SIZE) {
					for (i = 0; i < (int)(AES_BLOCK_SIZE - (len % AES_BLOCK_SIZE)) + 1; i++)//padding and padding_length
						es << (char)(AES_BLOCK_SIZE - (len % AES_BLOCK_SIZE));
				}
				else
					es << (char)0; //padding_length

				AES_KEY aes_e;
				if (AES_set_encrypt_key(pkeyw,
					(_cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA256 || _cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA) ? 256 : 128, &aes_e) < 0)
					return -1;

				RAND_bytes(IV, AES_BLOCK_SIZE);
				ss << rectype << (uint8_t)TLSVER_MAJOR << (uint8_t)TLSVER_NINOR << (uint16_t)0;
				ss.write(IV, AES_BLOCK_SIZE);
				AES_cbc_encrypt(es.data() + 13, ss.data() + ss.size(), es.size() - 13, &aes_e, IV, AES_ENCRYPT);
				ss.resize(ss.size() + es.size() - 13);
				ss.setpos(3) < (uint16_t)(es.size() + sizeof(IV) - 13);

				// output
				pout->append(ss.data(), ss.size());
				_seqno_send++;
				return (int)ss.size();
			}

			template <class _Out>
			bool mk_cipher(_Out *pout, uint8_t rectype, const uint8_t* pdata, size_t size)
			{
				size_t us = 0;
				while (us < size) {
					if (us + TLS_CBCBLKSIZE < size) {
						if (MKR_WithAES_BLK(pout, rectype, pdata + us, TLS_CBCBLKSIZE) < 0)
							return false;
						us += TLS_CBCBLKSIZE;
					}
					else {
						if (MKR_WithAES_BLK(pout, rectype, pdata + us, size - us) < 0)
							return false;
						break;
					}
				}
				return true;
			}

			template <class _Out>
			bool mk_nocipher(_Out *pout, int nprotocol, const void* pd, size_t size)
			{
				uint8_t s[8];
				const uint8_t *puc = (const uint8_t *)pd;
				size_t pos = 0, ss;

				s[0] = (uint8_t)nprotocol;
				s[1] = TLSVER_MAJOR;
				s[2] = TLSVER_NINOR;
				while (pos < size) {
					ss = TLS_CBCBLKSIZE;
					if (pos + ss > size)
						ss = size - pos;
					s[3] = (uint8_t)((ss >> 8) & 0xFF);
					s[4] = (uint8_t)(ss & 0xFF);
					pout->append(s, 5);
					pout->append(puc + pos, ss);
					pos += ss;
				}
				return true;
			}

			template <class _Out>
			bool make_package(_Out *pout, int nprotocol, const void* pd, size_t size)// make send package
			{
				if (_bsendcipher && *((uint8_t*)pd) != (uint8_t)tls::rec_alert)
					return mk_cipher(pout, (uint8_t)nprotocol, (const uint8_t*)pd, size);
				return mk_nocipher(pout, nprotocol, pd, size);
			}

			bool make_keyblock()
			{
				const char *slab = "key expansion";
				unsigned char seed[128];
				memcpy(seed, slab, strlen(slab));
				memcpy(&seed[strlen(slab)], _serverrand, 32);
				memcpy(&seed[strlen(slab) + 32], _clientrand, 32);
				if (!prf_sha256(_master_key, 48, seed, (int)strlen(slab) + 64, _key_block, 128))
					return false;
				SetCipherParam(_key_block, 128);
				return true;
			}

			template <class _Out>
			bool mkr_ClientFinished(_Out *pout)
			{
				const char* slab = "client finished";
				uint8_t hkhash[48];
				memcpy(hkhash, slab, strlen(slab));
				if (!_hmsg)
					return false;
				ec::vstream  tmp(8000);
				_hmsg->out(&tmp);
				uint8_t verfiy[32], sdata[32];
				SHA256(tmp.data(), tmp.size(), &hkhash[strlen(slab)]); //
				if (!prf_sha256(_master_key, 48, hkhash, (int)strlen(slab) + 32, verfiy, 32))
					return false;

				sdata[0] = tls::hsk_finished;
				sdata[1] = 0;
				sdata[2] = 0;
				sdata[3] = 12;
				memcpy(&sdata[4], verfiy, 12);

				_seqno_send = 0;
				_bsendcipher = true;

				if (make_package(pout, tls::rec_handshake, sdata, 16)) {
					_hmsg->_cli_finished.clear();
					_hmsg->_cli_finished.append(sdata, 16);
					return true;
				}
				return false;
			}

			template <class _Out>
			bool mkr_ServerFinished(_Out *pout)
			{
				const char* slab = "server finished";
				uint8_t hkhash[48];
				memcpy(hkhash, slab, strlen(slab));
				if (!_hmsg)
					return false;
				ec::vstream  tmp(8000);
				_hmsg->out(&tmp, true);
				uint8_t verfiy[32], sdata[32];
				SHA256(tmp.data(), tmp.size(), &hkhash[strlen(slab)]); //
				if (!prf_sha256(_master_key, 48, hkhash, (int)strlen(slab) + 32, verfiy, 32))
					return false;

				sdata[0] = tls::hsk_finished;
				sdata[1] = 0;
				sdata[2] = 0;
				sdata[3] = 12;
				memcpy(&sdata[4], verfiy, 12);

				_seqno_send = 0;
				_bsendcipher = true;

				return make_package(pout, tls::rec_handshake, sdata, 16);
			}

			template <class _Out>
			void Alert(uint8_t level, uint8_t desval, _Out* pout)
			{
				pout->clear();
				uint8_t u[8] = { (uint8_t)tls::rec_alert, TLSVER_MAJOR, TLSVER_NINOR, 0, 2, level, desval, 0 };
				pout->append(u, 7);
			}
		public:
			template <class _Out>
			bool MakeAppRecord(_Out* po, const void* pd, size_t size)
			{
				if (!_bhandshake_finished || !pd || !size)
					return false;
				po->clear();
				return make_package(po, tls::rec_application_data, pd, size);
			}

			virtual void Reset()
			{
				_bhandshake_finished = false;
				_breadcipher = false;
				_bsendcipher = false;

				_seqno_send = 0;
				_seqno_read = 0;
				_cipher_suite = 0;

				_pkgtcp.free();
				if (_hmsg)
					_hmsg->clear();
				else
					_hmsg = new handshake;
				resetblks();
			}

			static bool prf_sha256(const uint8_t* key, int keylen, const uint8_t* seed, int seedlen, uint8_t *pout, int outlen)
			{
				int nout = 0;
				uint32_t mdlen = 0;
				uint8_t An[32], Aout[32], An_1[32];
				if (!HMAC(EVP_sha256(), key, (int)keylen, seed, seedlen, An_1, &mdlen)) // A1
					return false;
				uint8_t as[1024];
				uint8_t *ps = (uint8_t *)as;
				while (nout < outlen) {
					memcpy(ps, An_1, 32);
					memcpy(ps + 32, seed, seedlen);
					if (!HMAC(EVP_sha256(), key, (int)keylen, ps, 32 + seedlen, Aout, &mdlen))
						return false;
					if (nout + 32 < outlen) {
						memcpy(pout + nout, Aout, 32);
						nout += 32;
					}
					else {
						memcpy(pout + nout, Aout, outlen - nout);
						nout = outlen;
						break;
					}
					if (!HMAC(EVP_sha256(), key, (int)keylen, An_1, 32, An, &mdlen)) // An
						return false;
					memcpy(An_1, An, 32);
				}
				return true;
			}

			void SetCipherParam(uint8_t *pkeyblock, int nsize)
			{
				memcpy(_keyblock, pkeyblock, nsize);
				if (_cipher_suite == TLS_RSA_WITH_AES_128_CBC_SHA256) {
					memcpy(_key_cwmac, _keyblock, 32);
					memcpy(_key_swmac, &_keyblock[32], 32);
					memcpy(_key_cw, &_keyblock[64], 16);
					memcpy(_key_sw, &_keyblock[80], 16);
				}
				else if (_cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA256) {
					memcpy(_key_cwmac, _keyblock, 32);
					memcpy(_key_swmac, &_keyblock[32], 32);
					memcpy(_key_cw, &_keyblock[64], 32);
					memcpy(_key_sw, &_keyblock[96], 32);
				}
				else if (_cipher_suite == TLS_RSA_WITH_AES_128_CBC_SHA) {
					memcpy(_key_cwmac, _keyblock, 20);
					memcpy(_key_swmac, &_keyblock[20], 20);
					memcpy(_key_cw, &_keyblock[40], 16);
					memcpy(_key_sw, &_keyblock[56], 16);
				}
				else if (_cipher_suite == TLS_RSA_WITH_AES_256_CBC_SHA) {
					memcpy(_key_cwmac, _keyblock, 20);
					memcpy(_key_swmac, &_keyblock[20], 20);
					memcpy(_key_cw, &_keyblock[40], 32);
					memcpy(_key_sw, &_keyblock[72], 32);
				}
			}

			template <class _Out>
			bool mkr_ClientHelloMsg(_Out* pout)
			{
				RAND_bytes(_clientrand, sizeof(_clientrand));
				if (!_hmsg)
					return false;
				_hmsg->_cli_hello.clear();
				try {
					_hmsg->_cli_hello << ((uint8_t)tls::hsk_client_hello);  // msg type 1byte
					_hmsg->_cli_hello << ((uint8_t)0) << (uint16_t)0; // msg len 3byte
					_hmsg->_cli_hello << (uint8_t)TLSVER_MAJOR << (uint8_t)TLSVER_NINOR;
					_hmsg->_cli_hello.write(_clientrand, 32);// random 32byte
					_hmsg->_cli_hello << (uint8_t)0;    // SessionID = NULL   1byte
					_hmsg->_cli_hello < (uint16_t)0x08; // 4 cipher_suites
					_hmsg->_cli_hello < (uint16_t)TLS_RSA_WITH_AES_256_CBC_SHA256;
					_hmsg->_cli_hello < (uint16_t)TLS_RSA_WITH_AES_128_CBC_SHA256;
					_hmsg->_cli_hello < (uint16_t)TLS_RSA_WITH_AES_256_CBC_SHA;
					_hmsg->_cli_hello < (uint16_t)TLS_RSA_WITH_AES_128_CBC_SHA;
					_hmsg->_cli_hello < (uint16_t)0x100; // compression_methods <1..2^8-1>
					_hmsg->_cli_hello.setpos(2);
					_hmsg->_cli_hello < (uint16_t)(_hmsg->_cli_hello.size() - 4);
				}
				catch (...) {
					return false;
				}
				return make_package(pout, tls::rec_handshake, _hmsg->_cli_hello.data(), _hmsg->_cli_hello.size());
			}

			/*!
			\brief do input bytes from tcp
			return <0 : error if pout not empty is sendback Alert pkg; >0: parse records and pout has decode message
			*/
			template <class _Out>
			int  OnTcpRead(const void* pd, size_t size, _Out* pout) // return TLS_SESSION_XXX
			{
				_pkgtcp.append((const uint8_t*)pd, size);
				uint8_t *p = (uint8_t*)_pkgtcp.data_(), uct, tmp[tls_rec_fragment_len + 2048];
				uint16_t ulen;
				int nl = (int)_pkgtcp.size_(), nret = TLS_SESSION_NONE, ndl = 0;
				while (nl >= 5) { // type(1byte) version(2byte) length(2byte);
					uct = *p;
					ulen = p[3];
					ulen = (ulen << 8) + p[4];
					if (uct < (uint8_t)tls::rec_change_cipher_spec || uct >(uint8_t)tls::rec_application_data ||
						p[1] != TLSVER_MAJOR || ulen > tls_rec_fragment_len + 64 || p[2] > TLSVER_NINOR) {
						if (_plog) {
							char stmp[1024];
							int nsize = nl > 128 ? 128 : nl;
							_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) TLS record error top128 %d bytes.\n%s", _ucid, nl,
								bin2view(p, nsize, stmp, sizeof(stmp)));
						}
						if (!_breadcipher)
							Alert(2, 70, pout);//protocol_version(70)
						return TLS_SESSION_ERR;
					}
					if (ulen + 5 > nl)
						break;
					if (_breadcipher) {
						if (decrypt_record(p, ulen + 5, &tmp[8], &ndl)) {
							nret = dorecord(&tmp[8], ndl, pout);
							if (nret == TLS_SESSION_ERR)
								return nret;
						}
						else {
							if (_plog) {
								char stmp[1024];
								int nsize = ulen + 5 > 128 ? 128 : ulen + 5;
								_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) Alert decode_error(50) : record size %u, top128\n%s", _ucid, ulen + 5,
									bin2view(p, nsize, stmp, sizeof(stmp)));
							}
							return TLS_SESSION_ERR;
						}
					}
					else {
						nret = dorecord(p, (int)ulen + 5, pout);
						if (nret == TLS_SESSION_ERR)
							return nret;
					}
					nl -= (int)ulen + 5;
					p += (int)ulen + 5;
				}
				_pkgtcp.freehead(_pkgtcp.size_() - nl);
				return nret;
			}
		protected:
			virtual int dorecord(const uint8_t* prec, size_t sizerec, bytes* pout) = 0;
		};

		class sessionclient : public session // session for client
		{
		public:
			sessionclient(uint32_t ucid, ilog* plog) : session(false, ucid, plog)
			{
				_prsa = nullptr;
				_pevppk = nullptr;
				_px509 = nullptr;
				_pkgm.reserve(TLS_REC_BUF_SIZE);
			}
			virtual ~sessionclient()
			{
				if (_prsa)
					RSA_free(_prsa);
				if (_pevppk)
					EVP_PKEY_free(_pevppk);
				if (_px509)
					X509_free(_px509);
				_prsa = nullptr;
				_pevppk = nullptr;
				_px509 = nullptr;
			}
		protected:
			RSA *_prsa;
			EVP_PKEY *_pevppk;
			X509* _px509;
			ec::bytes _pubkey;
		private:
			bytes _pkgm;
		public:
			bool SetServerPubkey(int len, const unsigned char *pubkey)
			{
				if (len > 8000)
					return false;
				_pubkey.assign(pubkey, len);
				return true;
			}

			bool SetServerCa(const char* scafile)
			{
				ec::string pkey;
				if (!get_cert_pkey(scafile, &pkey))
					return false;
				return SetServerPubkey((int)pkey.size(), (const unsigned char*)pkey.data());
			}

			virtual void Reset()
			{
				session::Reset();
				if (_prsa)
					RSA_free(_prsa);
				if (_pevppk)
					EVP_PKEY_free(_pevppk);
				if (_px509)
					X509_free(_px509);
				_prsa = nullptr;
				_pevppk = nullptr;
				_px509 = nullptr;
				_pkgm.clear();
			}

		private:
			template <class _Out>
			bool mkr_ClientKeyExchange(_Out *po)
			{
				if (!_hmsg)
					return false;
				unsigned char premasterkey[48], out[512];
				premasterkey[0] = TLSVER_MAJOR;
				premasterkey[1] = TLSVER_NINOR;
				RAND_bytes(&premasterkey[2], 46); //calculate pre_master_key

				const char* slab = "master secret";//calculate master_key
				unsigned char seed[128];
				memcpy(seed, slab, strlen(slab));
				memcpy(&seed[strlen(slab)], _clientrand, 32);
				memcpy(&seed[strlen(slab) + 32], _serverrand, 32);
				if (!prf_sha256(premasterkey, 48, seed, (int)strlen(slab) + 64, _master_key, 48))
					return false;

				if (!make_keyblock()) //calculate key_block
					return false;

				int nbytes = RSA_public_encrypt(48, premasterkey, out, _prsa, RSA_PKCS1_PADDING);
				if (nbytes < 0)
					return false;
				_hmsg->_cli_key_exchange.clear();
				uint8_t uh[6] = {
					(uint8_t)(tls::hsk_client_key_exchange), // msgtype
					(uint8_t)0, (uint8_t)(((uint32_t)(nbytes + 2) >> 8) & 0xFF), (uint8_t)((uint32_t)(nbytes + 2) & 0xFF), // 3bytes length
					(uint8_t)(((uint32_t)nbytes >> 8) & 0xFF), (uint8_t)((uint32_t)nbytes & 0xFF) // 2byte  secrit premasterkey length
				};
				_hmsg->_cli_key_exchange.append(uh, 6);
				_hmsg->_cli_key_exchange.append(out, nbytes); // secrit premasterkey
				return make_package(po, tls::rec_handshake, _hmsg->_cli_key_exchange.data(), _hmsg->_cli_key_exchange.size());
			}

			bool OnServerHello(unsigned char* phandshakemsg, size_t size)
			{
				if (!_hmsg)
					return false;
				_hmsg->_srv_hello.clear();
				_hmsg->_srv_hello.append(phandshakemsg, size);

				if (_hmsg->_srv_hello.size() < 40u)
					return false;
				unsigned char* puc = _hmsg->_srv_hello.data();
				uint32_t ulen = puc[1];
				ulen = (ulen << 8) + puc[2];
				ulen = (ulen << 8) + puc[3];

				puc += 6;
				memcpy(_serverrand, puc, 32);
				puc += 32;

				int n = *puc++; // sessionID
				puc += n;

				if (n + 40 > (int)_hmsg->_srv_hello.size())
					return false;

				_cipher_suite = *puc++;
				_cipher_suite = (_cipher_suite << 8) | *puc++;
				return true;
			}

			bool OnServerCertificate(unsigned char* phandshakemsg, size_t size)
			{
				if (!_hmsg)
					return false;
				_hmsg->_srv_certificate.clear();
				_hmsg->_srv_certificate.append(phandshakemsg, size);

				if (!_hmsg->_srv_certificate.size())
					return false;
				const unsigned char* p = _hmsg->_srv_certificate.data();//, *pend = 0;

				uint32_t ulen = p[7];
				ulen = (ulen << 8) + p[8];
				ulen = (ulen << 8) + p[9];
				p += 10;
				_px509 = d2i_X509(NULL, &p, (long)ulen);//only use first Certificate
				if (!_px509)
					return false;

				if (!_pubkey.empty()) { // verify the server legitimacy
					ASN1_BIT_STRING* pstr = X509_get0_pubkey_bitstr(_px509);
					if (pstr->length != (int)_pubkey.size() || memcmp(pstr->data, _pubkey.data(), _pubkey.size())) {
						X509_free(_px509);
						_px509 = nullptr;
						return false;
					}
				}

				_pevppk = X509_get_pubkey(_px509);
				if (!_pevppk) {
					X509_free(_px509);
					_px509 = nullptr;
					return false;
				}
				_prsa = EVP_PKEY_get1_RSA(_pevppk);//get copy of RSA
				if (!_prsa) {
					EVP_PKEY_free(_pevppk);
					X509_free(_px509);
					_pevppk = nullptr;
					_px509 = nullptr;
					return false;
				}
				return  true;
			}

			template <class _Out>
			bool  OnServerHelloDone(uint8_t* phandshakemsg, size_t size, _Out* pout)
			{
				if (!_hmsg)
					return false;
				_hmsg->_srv_hellodone.clear();
				_hmsg->_srv_hellodone.append(phandshakemsg, size);
				if (!mkr_ClientKeyExchange(pout))
					return false;
				unsigned char change_cipher_spec = 1;// send change_cipher_spec
				make_package(pout, tls::rec_change_cipher_spec, &change_cipher_spec, 1);
				if (!mkr_ClientFinished(pout))
					return false;
				return true;
			}

			template <class _Out>
			bool OnServerFinished(uint8_t* phandshakemsg, size_t size, _Out* pout)
			{
				if (!_hmsg)
					return false;
				const char* slab = "server finished";
				uint8_t hkhash[48];
				memcpy(hkhash, slab, strlen(slab));
				ec::vstream tmp(8000);
				_hmsg->out(&tmp, true);
				uint8_t verfiy[32];
				SHA256(tmp.data(), tmp.size(), &hkhash[strlen(slab)]); //
				if (!prf_sha256(_master_key, 48, hkhash, (int)strlen(slab) + 32, verfiy, 32))
					return false;

				int i;
				for (i = 0; i < 12; i++) {
					if (verfiy[i] != phandshakemsg[4 + i]) {
						Alert(2, 40, pout);//handshake_failure(40)
						return false;
					}
				}
				delete _hmsg;//Handshake completed, delete Handshake message
				_hmsg = nullptr;
				return true;
			}

		protected:
			virtual int dorecord(const uint8_t* prec, size_t sizerec, bytes * pout) // return TLS_SESSION_XXX
			{
				const uint8_t* p = (const uint8_t*)prec;
				uint16_t ulen = p[3];
				ulen = (ulen << 8) + p[4];

				if (p[0] == tls::rec_handshake)
					return dohandshakemsg(p + 5, sizerec - 5, pout);
				else if (p[0] == tls::rec_alert) {
					if (_plog) {
						char so[512];
						_plog->add(CLOG_DEFAULT_WRN, "TLS client Alert level = %d, AlertDescription = %d,size = %zu\n%s", p[5], p[6], sizerec,
							bin2view(prec, sizerec, so, sizeof(so)));
					}
				}
				else if (p[0] == tls::rec_change_cipher_spec) {
					_breadcipher = true;
					_seqno_read = 0;
					if (_plog)
						_plog->add(CLOG_DEFAULT_DBG, "TLS client server change_cipher_spec");
				}
				else if (p[0] == tls::rec_application_data) {
					pout->append(p + 5, (int)sizerec - 5);
					return TLS_SESSION_APPDATA;
				}
				return TLS_SESSION_NONE;
			}

			template <class _Out>
			int dohandshakemsg(const uint8_t* prec, size_t sizerec, _Out* pout)
			{
				_pkgm.append((const unsigned char*)prec, sizerec);
				int nl = (int)_pkgm.size(), nret = TLS_SESSION_NONE;
				unsigned char* p = _pkgm.data();
				while (nl >= 4) {
					uint32_t ulen = p[1];
					ulen = (ulen << 8) + p[2];
					ulen = (ulen << 8) + p[3];
					if (ulen > 1024 * 16)
						return TLS_SESSION_ERR;
					if ((int)ulen + 4 > nl)
						break;
					switch (p[0]) {
					case tls::hsk_server_hello:
						if (!OnServerHello(p, ulen + 4)) {
							if (_plog)
								_plog->add(CLOG_DEFAULT_DBG, "TLS client sever hello package error, size=%u", ulen + 4);
							return TLS_SESSION_ERR;
						}
						break;
					case tls::hsk_certificate:
						if (!OnServerCertificate(p, ulen + 4))
							return TLS_SESSION_ERR;
						break;
					case tls::hsk_server_key_exchange:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "TLS client hsk_server_key_exchange size=%u", ulen + 4);
						break;
					case tls::hsk_certificate_request:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "TLS client hsk_certificate_request size=%u", ulen + 4);
						break;
					case tls::hsk_server_hello_done:
						if (!OnServerHelloDone(p, ulen + 4, pout))
							return TLS_SESSION_ERR;
						break;
					case tls::hsk_finished:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "TLS client hsk_finished size=%u", ulen + 4);
						if (!OnServerFinished(p, ulen + 4, pout))
							return TLS_SESSION_ERR;
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "TLS client server hsk_finished check success");
						_bhandshake_finished = true;
						nret = TLS_SESSION_HKOK;
						break;
					default:
						if (_plog)
							_plog->add(CLOG_DEFAULT_ERR, "TLS client unkown msgtype = %u", p[0]);
						return TLS_SESSION_ERR;
					}
					nl -= (int)ulen + 4;
					p += (int)ulen + 4;
				}
				_pkgm.erase(0, _pkgm.size() - nl);
				if(_pkgm.capacity() > 1024 * 18)
					_pkgm.shrink_to_fit();
				return nret;
			}
		};

		class sessionserver : public session // session for server
		{
		public:
			sessionserver(uint32_t ucid, ec::tls_srvca* pca, ilog* plog
			) : session(true, ucid, plog)
			{
				_pca = pca;
				memset(_sip, 0, sizeof(_sip));
			}

			sessionserver(sessionserver* p) : session(p)
			{
				_pca = p->_pca;
				memcpy(_sip, p->_sip, sizeof(_sip));
				_pkgm.append(p->_pkgm.data_(), p->_pkgm.size_());
			}

			sessionserver(sessionserver&& v) : session(std::move(v)), _pkgm(std::move(v._pkgm))
			{
				_pca = v._pca;
				memcpy(_sip, v._sip, sizeof(_sip));
			}

			virtual ~sessionserver()
			{
			}
		protected:
			ec::tls_srvca* _pca;
			char _sip[32];
		private:
			parsebuffer _pkgm;// for handshake
		public:
			inline void SetIP(const char* sip)
			{
				strlcpy(_sip, sip, sizeof(_sip));
			}

			inline void getip(char *sout, size_t sizeout)
			{
				strlcpy(sout, _sip, sizeout);
			}

		protected:
			bool MakeServerHello()
			{
				if (!_hmsg)
					return false;
				RAND_bytes(_serverrand, sizeof(_serverrand));
				_hmsg->_srv_hello.clear();
				try {
					_hmsg->_srv_hello << (uint8_t)tls::hsk_server_hello;
					_hmsg->_srv_hello << (uint16_t)0 << (uint8_t)0;
					_hmsg->_srv_hello << (uint8_t)TLSVER_MAJOR << (uint8_t)TLSVER_NINOR;
					_hmsg->_srv_hello.write(_serverrand, 32);// random 32byte
					_hmsg->_srv_hello << (uint8_t)4;
					_hmsg->_srv_hello < _ucid;
					_hmsg->_srv_hello << (uint8_t)0;
					_hmsg->_srv_hello << (uint8_t)(_cipher_suite & 0xFF);
					_hmsg->_srv_hello << (uint8_t)0;//compression_methods.null
				}
				catch (...) {
					return false;
				}
				*(_hmsg->_srv_hello.data() + 3) = (uint8_t)(_hmsg->_srv_hello.size() - 4);
				return true;
			}

			bool MakeCertificateMsg()
			{
				if (!_hmsg)
					return false;
				return _pca->MakeCertificateMsg(_hmsg->_srv_certificate);
			}

			template <class _Out>
			bool OnClientHello(uint8_t* phandshakemsg, size_t size, _Out* po)
			{
				if (!_hmsg)
					return false;
				_hmsg->_cli_hello.clear();
				_hmsg->_cli_hello.append(phandshakemsg, size);

				unsigned char* puc = phandshakemsg, uct;
				size_t ulen = puc[1];
				ulen = (ulen << 8) + puc[2];
				ulen = (ulen << 8) + puc[3];

				if (size != ulen + 4 || size < 12 + 32) {
					Alert(2, 10, po);//unexpected_message(10)
					return false;
				}
				if (puc[4] != TLSVER_MAJOR || puc[5] < TLSVER_NINOR) {
					if (_plog)
						_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) client Hello Ver %d.%d", _ucid, puc[4], puc[5]);
					Alert(2, 70, po);//protocol_version(70),
					return false;
				}
				stream ss(phandshakemsg, size);
				unsigned short i, cipherlen = 0;
				try {
					ss.setpos(6).read(_clientrand, 32) >> uct; //session id len
					if (uct > 0)
						ss.setpos(ss.getpos() + uct);
					ss > cipherlen;
				}
				catch (int) {
					return false;
				}
				if (ss.getpos() + cipherlen > size) {
					Alert(2, 10, po);//unexpected_message(10)
					return false;
				}
				_cipher_suite = 0;
				unsigned char* pch = phandshakemsg + ss.getpos();
				if (_plog) {
					char so[512];
					_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) client ciphers : \n%s ", _ucid, bin2view(pch, cipherlen, so, sizeof(so)));
				}
				for (i = 0; i < cipherlen; i += 2) {
					if (pch[i] == 0 && (pch[i + 1] == TLS_RSA_WITH_AES_128_CBC_SHA256 || pch[i + 1] == TLS_RSA_WITH_AES_256_CBC_SHA256
						|| pch[i + 1] == TLS_RSA_WITH_AES_128_CBC_SHA || pch[i + 1] == TLS_RSA_WITH_AES_256_CBC_SHA)
						) {
						_cipher_suite = pch[i + 1];
						break;
					}
				}
				if (!_cipher_suite) {
					Alert(2, 40, po);//handshake_failure(40)
					return false;
				}
				if (_plog)
					_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) server cipher = (%02x,%02x)", _ucid, (_cipher_suite >> 8) & 0xFF, _cipher_suite & 0xFF);

				MakeServerHello();
				MakeCertificateMsg();
				uint8_t umsg[4] = { tls::hsk_server_hello_done, 0, 0, 0 };
				make_package(po, tls::rec_handshake, _hmsg->_srv_hello.data(), _hmsg->_srv_hello.size());// ServerHello
				make_package(po, tls::rec_handshake, _hmsg->_srv_certificate.data(), _hmsg->_srv_certificate.size());//Certificate
				_hmsg->_srv_hellodone.clear();
				_hmsg->_srv_hellodone.append(umsg, 4);
				make_package(po, tls::rec_handshake, umsg, 4);//ServerHelloDone
				return true;
			}

			template <class _Out>
			bool OnClientKeyExchange(const uint8_t* pmsg, size_t sizemsg, _Out* po)
			{
				if (!_hmsg)
					return false;
				_hmsg->_cli_key_exchange.clear();
				_hmsg->_cli_key_exchange.append(pmsg, sizemsg);

				uint32_t ulen = pmsg[1];//private key decode
				ulen = (ulen << 8) | pmsg[2];
				ulen = (ulen << 8) | pmsg[3];

				if (ulen + 4 != sizemsg) {
					Alert(2, 10, po);//unexpected_message(10)
					return false;
				}

				int nbytes = 0;
				unsigned char premasterkey[48];
				if (ulen % 16) { //规范本版本
					uint32_t ul = pmsg[4];//private key decode
					ul = (ul << 8) | pmsg[5];
					nbytes = _pca->PrivateDecrypt((int)ul, pmsg + 6, premasterkey);
				}
				else { //兼容错误版本
					nbytes = _pca->PrivateDecrypt((int)ulen, pmsg + 4, premasterkey);
				}

				if (nbytes != 48) {
					Alert(2, 21, po);//decryption_failed(21),
					return false;
				}

				const char* slab = "master secret";//calculate master_key
				uint8_t seed[128];
				memcpy(seed, slab, strlen(slab));
				memcpy(&seed[strlen(slab)], _clientrand, 32);
				memcpy(&seed[strlen(slab) + 32], _serverrand, 32);
				if (!prf_sha256(premasterkey, 48, seed, (int)strlen(slab) + 64, _master_key, 48)) {
					Alert(2, 80, po);//internal_error(80),
					return false;
				}

				if (!make_keyblock()) { //calculate key_block
					Alert(2, 80, po);//internal_error(80),
					return false;
				}
				return true;
			}

			template <class _Out>
			bool OnClientFinish(const uint8_t* pmsg, size_t sizemsg, _Out* po)
			{
				if (!_hmsg)
					return false;
				const char* slab = "client finished";
				uint8_t hkhash[48];
				memcpy(hkhash, slab, strlen(slab));
				ec::vstream  tmp(8000);
				_hmsg->out(&tmp);
				unsigned char verfiy[32];
				SHA256(tmp.data(), tmp.size(), &hkhash[strlen(slab)]); //
				if (!prf_sha256(_master_key, 48, hkhash, (int)strlen(slab) + 32, verfiy, 32)) {
					Alert(2, 80, po);//internal_error(80),
					return false;
				}

				size_t len = pmsg[1];
				len = (len << 8) | pmsg[2];
				len = (len << 8) | pmsg[3];

				if (len + 4 != sizemsg || len != 12) {
					Alert(2, 10, po);//unexpected_message(10)
					return false;
				}
				int i;
				for (i = 0; i < 12; i++) {
					if (verfiy[i] != pmsg[4 + i]) {
						Alert(2, 40, po);//handshake_failure(40)
						return false;
					}
				}

				unsigned char change_cipher_spec = 1;//send change_cipher_spec
				make_package(po, tls::rec_change_cipher_spec, &change_cipher_spec, 1);

				_seqno_send = 0;
				_bsendcipher = true;
				_hmsg->_cli_finished.clear();
				_hmsg->_cli_finished.append(pmsg, sizemsg);
				if (_plog)
					_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) rec_change_cipher_spec success!", _ucid);
				if (!mkr_ServerFinished(po))
					return false;
				if (_plog)
					_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) ClientFinished success!", _ucid);
				delete _hmsg;//Handshake completed, delete Handshake message
				_hmsg = nullptr;
				return true;
			}

			virtual int dorecord(const uint8_t* prec, size_t sizerec, bytes* po) // return TLS_SESSION_XXX
			{
				const unsigned char* p = (const unsigned char*)prec;
				uint16_t ulen = p[3];
				ulen = (ulen << 8) + p[4];

				if (p[0] == tls::rec_handshake)
					return dohandshakemsg(p + 5, sizerec - 5, po);
				else if (p[0] == tls::rec_alert) {
					if (_plog) {
						char so[512];
						_plog->add(CLOG_DEFAULT_WRN, "ucid(%u) Alert level = %d,AlertDescription = %d,size = %zu\n%s", _ucid, p[5], p[6], sizerec,
							bin2view(prec, sizerec, so, sizeof(so)));
					}
				}
				else if (p[0] == tls::rec_change_cipher_spec) {
					_breadcipher = true;
					_seqno_read = 0;
					if (_plog)
						_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) server change_cipher_spec", _ucid);
				}
				else if (p[0] == tls::rec_application_data) {
					po->append(p + 5, sizerec - 5u);
					return TLS_SESSION_APPDATA;
				}
				return TLS_SESSION_NONE;
			}

			template <class _Out>
			int dohandshakemsg(const uint8_t* prec, size_t sizerec, _Out* po)
			{
				_pkgm.append((const unsigned char*)prec, sizerec);
				int nl = (int)_pkgm.size_(), nret = TLS_SESSION_NONE;
				unsigned char* p = (unsigned char*)_pkgm.data_();
				while (nl >= 4) {
					uint32_t ulen = p[1];
					ulen = (ulen << 8) + p[2];
					ulen = (ulen << 8) + p[3];
					if (ulen > 8192) {
						if (_plog)
							_plog->add(CLOG_DEFAULT_ERR, "ucid(%u) read handshake message datasize error size=%u", _ucid, ulen);
						return TLS_SESSION_ERR;
					}
					if ((int)ulen + 4 > nl)
						break;
					switch (p[0]) {
					case tls::hsk_client_hello:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) read hsk_client_hello size=%u", _ucid, ulen + 4);
						if (!OnClientHello(p, ulen + 4, po)) {
							if (_plog)
								_plog->add(CLOG_DEFAULT_ERR, "ucid(%u) client hsk_client_hello failed", _ucid);
							return -1;
						}
						break;
					case tls::hsk_client_key_exchange:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) read hsk_client_key_exchange size=%u", _ucid, ulen + 4);
						if (!OnClientKeyExchange(p, ulen + 4, po)) {
							if (_plog)
								_plog->add(CLOG_DEFAULT_ERR, "ucid(%u) client hsk_client_key_exchange failed", _ucid);
							return TLS_SESSION_ERR;
						}
						break;
					case tls::hsk_finished:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) read hsk_finished size=%u", _ucid, ulen + 4);
						if (!OnClientFinish(p, ulen + 4, po)) {
							if (_plog)
								_plog->add(CLOG_DEFAULT_ERR, "ucid(%u) client hsk_finished failed", _ucid);
							return -1;
						}
						_bhandshake_finished = true;
						nret = TLS_SESSION_HKOK;
						break;
					default:
						if (_plog)
							_plog->add(CLOG_DEFAULT_DBG, "ucid(%u) unkown msgtype=%u", _ucid, p[0]);
						return TLS_SESSION_ERR;
					}
					nl -= (int)ulen + 4;
					p += (int)ulen + 4;
				}
				if (TLS_SESSION_HKOK == nret)
					_pkgm.free();
				else
					_pkgm.freehead(_pkgm.size_() - nl);
				return nret;
			}
		};

	}// tls
}// ec
