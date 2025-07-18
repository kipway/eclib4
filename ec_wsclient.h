﻿/*!
\file ec_wsclient.h
\author	jiangyong
\email  kipway@outlook.com
\update
    2025.3.11 fix ParseOneFrame
  2023.8.10 update ,add request failed http start line out to log
  2023.7.5  remove ec::memory
  2023.6.25 add sendPingMsgMsg
ws_c
	class for websocket client
	tcp_c -> ws_c

eclib 4.0 Copyright (c) 2017-2025, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include "ec_log.h"
#include "ec_sha1.h"
#include "ec_base64.h"
#include "ec_guid.h"
#include "ec_tcpclient.h"
#include "ec_wstips.h"
#include "ec_http.h"

namespace ec
{
	class websocketclient
	{
	public:
		websocketclient(ilog* plog)
			: _plog(plog)
			, _comp(0)
			, _opcode(WS_OP_TXT)

		{
			_srequrl[0] = '/';
			_srequrl[1] = 0;
			_shost[0] = 0;
			_sprotocol[0] = 0;
			_sb64ret[0] = 0;
			_wscompress = ws_permessage_deflate;
			_umask = (uint32_t)::time(nullptr);
			_wsmsg.reserve(1000 * 16);
		}
	protected:
		ilog* _plog;
		cGuid _guid;
		char  _srequrl[80];
		char _shost[64];
		char _sb64[40], _sb64ret[40];
		char _sprotocol[40];
	private:
		int _wscompress; // ws_x_webkit_deflate_frame or ws_permessage_deflate
		bytes _wsmsg; // ws frame
		int _comp;// compress flag
		int _opcode;  // operate code
		uint32_t _umask;
		uint32_t nextmask()
		{
			_umask++;
			if (!_umask)
				_umask = 1;
			return _umask * 2654435769U;
		}
	public:
		void init(const char* srequrl, const char* shost, const char* sprotocol)
		{
			if (!srequrl || !*srequrl) {
				_srequrl[0] = '/';
				_srequrl[1] = 0;
			}
			else {
				if (*srequrl == '/')
					ec::strlcpy(_srequrl, srequrl, sizeof(_srequrl));
				else {
					_srequrl[0] = '/';
					_srequrl[1] = 0;
					ec::strlcpy(&_srequrl[1], srequrl, sizeof(_srequrl) - 1u);
				}
			}
			if (shost && *shost)
				ec::strlcpy(_shost, shost, sizeof(_shost));
			if (sprotocol && *sprotocol)
				ec::strlcpy(_sprotocol, sprotocol, sizeof(_sprotocol));
			else
				_sprotocol[0] = 0;
		}

		template<class _Out>
		bool makeRequest(_Out& pkg)
		{
			const char* sc;
			pkg.append("GET ", 4);
			pkg.append(_srequrl, strlen(_srequrl));
			sc = " HTTP/1.1 \r\n";
			pkg.append(sc, strlen(sc));

			if (_shost[0]) {
				sc = "host: ";
				pkg.append(sc, strlen(sc));
				pkg.append(_shost, strlen(_shost));
				pkg.append("\r\n", 2);
			}

			sc = "Connection: Upgrade\r\nUpgrade: websocket\r\n";
			pkg.append(sc, strlen(sc));
			sc = "Sec-WebSocket-Version: 13\r\n";
			pkg.append(sc, strlen(sc));

			t_guid uid;
			_guid.uuid(&uid);
			ec::encode_base64(_sb64, (const char*)&uid, 16);
			sc = "Sec-WebSocket-Key: ";
			pkg.append(sc, strlen(sc));
			pkg.append(_sb64, strlen(_sb64));
			pkg.append("\r\n", 2);

			char tmp[80] = { 0 };
			char sha1out[24];
			strcpy(tmp, _sb64);
			strcat(tmp, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
			encode_sha1(tmp, (unsigned int)strlen(tmp), sha1out);
			encode_base64(_sb64ret, sha1out, 20);

			if (_sprotocol[0]) {
				sc = "Sec-WebSocket-Protocol: ";
				pkg.append(sc, strlen(sc));
				pkg.append(_sprotocol, strlen(_sprotocol));
				pkg.append("\r\n", 2);
			}

			sc = "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";
			pkg.append(sc, strlen(sc));

			pkg.append("\r\n", 2);
			return true;
		}

		int doRequest(parsebuffer& rin)
		{
			http::package htp;
			int nlen = htp.parse((const char*)rin.data_(), rin.size_());
			if (nlen < 0) {
				return -1;
				if (_plog)
					_plog->add(CLOG_DEFAULT_ERR, "error http package.");
				return -1;
			}
			else if (nlen == 0)
				return 0;
			char sacp[40];
			if (!htp.GetHeadFiled("Sec-WebSocket-Accept", sacp, sizeof(sacp))
				|| !ec::streq(sacp, _sb64ret)) {
				if (_plog) {
					int nout = rin.size_() > 200 ? 200 : (int)rin.size_();
					_plog->add(CLOG_DEFAULT_ERR, "Sec-WebSocket-Accept failed\n%.*s", nout, (const char*)rin.data_());
				}
				return -1;
			}
			if (htp.CheckHeadFiled("Sec-WebSocket-Extensions", "permessage-deflate"))
				_wscompress = ws_permessage_deflate;
			else if (htp.CheckHeadFiled("Sec-WebSocket-Extensions", "x-webkit-deflate-frame"))
				_wscompress = ws_x_webkit_deflate_frame;
			else
				_wscompress = 0; //no compress
			rin.freehead(nlen);
			return 1;
		}

		template<class _Out>
		int doWsData(parsebuffer& rbuf, _Out* pmsgout, int* popcode = nullptr)// return -1:error; 0: no msg; 1:one msg
		{
			size_t sizedo = 0;
			pmsgout->clear();
			int nr = WebsocketParse((const char*)rbuf.data_(), rbuf.size_(), sizedo, pmsgout, popcode);//websocket
			if (nr == he_failed) {
				rbuf.free();
				return -1;
			}
			else {
				if (sizedo) {
					rbuf.freehead(sizedo);
				}
			}
			return nr == he_ok? 1 : 0;
		}

		template<class _Out = vstream>
		int makeWsPackage(const void* p, size_t size, _Out* pout, int opcode = WS_OP_TXT)
		{
			if (_wscompress == ws_x_webkit_deflate_frame)  //deflate-frame
				return ws_make_perfrm(p, size, opcode, pout, nextmask()) ? 0 : -1;
			return  ws_make_permsg(p, size, opcode, pout, size > 128 && _wscompress, nextmask()) ? 0 : -1;// ws_permessage_deflate
		}

	private:
		void reset_msg()
		{
			_wsmsg.clear();
			if (_wsmsg.capacity() > 1024 * 32)
				_wsmsg.shrink_to_fit();
			_comp = 0;
			_opcode = WS_OP_TXT;
		}

		int  ParseOneFrame(const char* stxt, size_t usize, int& fin)// reuturn >0 is do bytes; -1:error; 0:wait
		{
			int comp = 0;
			fin = 0;
			if (usize < 2)
				return 0;
			int i;
			size_t datalen = 0, sizedo = 0;
			size_t datapos = 2;
			unsigned char* pu = (unsigned char*)stxt;

			fin = pu[0] & 0x80;
			comp = (pu[0] & 0x40) ? 1 : 0;
			int bmask = pu[1] & 0x80;
			int payloadlen = pu[1] & 0x7F;

			if (!_wsmsg.size())
				_opcode = pu[0] & 0x0f;

			if (bmask)//client can not use mask
				datapos += 4;

			if (payloadlen == 126) {
				datapos += 2;
				if (usize < datapos)
					return 0;

				datalen = pu[2];
				datalen <<= 8;
				datalen |= pu[3];
			}
			else if (payloadlen == 127) {
				datapos += 8;
				if (usize < datapos)
					return 0;

				for (i = 0; i < 8; i++) {
					if (i > 0)
						datalen <<= 8;
					datalen |= pu[2 + i];
				}
			}
			else {
				datalen = payloadlen;
				if (usize < datapos)
					return 0;
			}
			if (datalen > MAXSIZE_WS_READ_FRAME || _wsmsg.size() + datalen > MAXSIZE_WS_READ_PKG)//outof size limit
				return -1;
			if (usize < datapos + datalen)
				return 0;
			if (bmask) {
				unsigned int umask = pu[datapos - 1];
				umask <<= 8;
				umask |= pu[datapos - 2];
				umask <<= 8;
				umask |= pu[datapos - 3];
				umask <<= 8;
				umask |= pu[datapos - 4];
				xor_le(pu + datapos, (int)datalen, umask);
			}
			sizedo = datapos + datalen;

			if (!comp)
				_wsmsg.append((uint8_t*)stxt + datapos, datalen);
			else {
				if (_wscompress == ws_x_webkit_deflate_frame) { //deflate_frame
					bytes debuf;
					debuf.reserve(EC_SIZE_WS_FRAME);
					debuf.push_back('\x78');
					debuf.push_back('\x9c');
					debuf.append((uint8_t*)stxt + datapos, datalen);
					if (!_wsmsg.size()) {
						if (Z_OK != ws_decode_zlib(debuf.data(), debuf.size(), &_wsmsg))
							return -1;
					}
					else {
						bytes tmp;
						tmp.reserve(4 * debuf.size());
						if (Z_OK != ws_decode_zlib(debuf.data(), debuf.size(), &tmp))
							return -1;
						_wsmsg.append(tmp.data(), tmp.size());
					}
				}
				else {
					_comp = 1;
					_wsmsg.clear();
					_wsmsg.push_back('\x78');
					_wsmsg.push_back('\x9c');
					_wsmsg.append((uint8_t*)stxt + datapos, datalen);
				}
			}
			return (int)sizedo;
		}
		template<class _Out = bytes>
		int WebsocketParse(const char* stxt, size_t usize, size_t& sizedo, _Out* pout, int* popcode = nullptr)//support multi-frame
		{
			const char* pd = stxt;
			int ndo = 0, fin = 0;
			sizedo = 0;
			while (sizedo < usize) {
				ndo = ParseOneFrame(pd, usize - sizedo, fin);
				if (ndo <= 0)
					break;
				sizedo += ndo;
				pd += ndo;
				if (fin) {// end frame
					pout->clear();
					if (_comp && _wscompress == ws_permessage_deflate) {
						if (Z_OK != ws_decode_zlib(_wsmsg.data(), _wsmsg.size(), pout)) {
							pout->clear();
							return he_failed;
						}
					}
					else
						pout->append((uint8_t*)_wsmsg.data(), _wsmsg.size());
					if (WS_OP_PONG == _opcode) {
						pout->clear();
						reset_msg();
						return he_waitdata;
					}
					if (popcode)
						*popcode = _opcode;
					reset_msg();
					return he_ok;
				}
			}
			pout->clear();
			if (ndo < 0)
				return he_failed;
			return he_waitdata;
		}
	};//websocketclient

	class ws_c : public tcp_c
	{
	public:
		ws_c(ilog* plog) : _nstatus(0) , _plog(plog) , _ws(plog)
		{
		}

		int get_ws_status()
		{
			return _nstatus;
		}
	private:
		int _nstatus;// 0: none handshaked;  1:handshaked
	protected:
		ilog* _plog;
	private:
		parsebuffer _rbuf;
		websocketclient _ws;

	protected:
		virtual void onwshandshake() = 0;
		virtual int onwsdata(const uint8_t* p, int nbytes) = 0;//return 0:OK ; -1:error will disconnect;
	public:
		inline void initws(const char* srequrl, const char* shost, const char* sprotocol)
		{
			_ws.init(srequrl, shost, sprotocol);
		}

		virtual int sendbytes(const void* p, int nlen)
		{
			if (!_nstatus)
				return tcp_c::sendbytes(p, nlen);
			vstream vs;
			vs.reserve(1024 + nlen - nlen % 512);
			if (_ws.makeWsPackage(p, nlen, &vs) < 0) {
				close();
				return -1;
			}
			return tcp_c::sendbytes(vs.data(), (int)vs.size());
		}
		/**
		 * @brief send ping messgae
		 * @param sutf8 text to send
		 * @return -1: error; 0:none handshaked; >0 package size sended;
		*/
		int sendPingMsg(const char* sutf8)
		{
			if (!_nstatus)
				return 0;
			const char* s = (sutf8 && *sutf8) ? sutf8 : "ping";
			vstream vs;
			if (_ws.makeWsPackage(s, strlen(s), &vs, WS_OP_PING) < 0) {
				close();
				return -1;
			}
			return tcp_c::sendbytes(vs.data(), (int)vs.size());
		}
	protected:
		virtual void onconnected()
		{
			tcp_c::onconnected();
			bytes pkg;
			pkg.reserve(500);
			_ws.makeRequest(pkg);
			tcp_c::sendbytes(pkg.data(), (int)pkg.size());
		}

		virtual void ondisconnected()
		{
			_nstatus = 0;
			_rbuf.free();
		}

		virtual void onreadbytes(const uint8_t* p, int nbytes)
		{
			int nr = 0, nopcode = 0;
			bytes pkg;
			_rbuf.append(p, nbytes);
			if (!_nstatus) {
				nr = _ws.doRequest(_rbuf);
				if (nr < 0) {
					close();
					return;
				}
				else if (nr == 0)
					return;
				_nstatus = 1;
				onwshandshake();
				if (_rbuf.empty())
					return;
			}
			nr = _ws.doWsData(_rbuf, &pkg, &nopcode);
			while (1 == nr) {
				if (WS_OP_PING == nopcode) {
					if (sendwsbytes(pkg.data(), (int)pkg.size(), WS_OP_PONG) < 0)
						return;
				}
				else if(WS_OP_TXT == nopcode) {
					nr = onwsdata(pkg.data(), (int)pkg.size());
					if (nr < 0)
						break;
				}
				pkg.clear();
				nopcode = 0;
				nr = _ws.doWsData(_rbuf, &pkg, &nopcode);
			}
			if (nr < 0) {
				close();
				_nstatus = 0;
			}
		}

		int sendwsbytes(const void* p, int nlen, int opcode)
		{
			vstream vs;
			vs.reserve(1024 + nlen - nlen % 512);
			if (_ws.makeWsPackage(p, nlen, &vs, opcode) < 0) {
				close();
				return -1;
			}
			return tcp_c::sendbytes(vs.data(), (int)vs.size());
		}
	};//ws_c
}//ec
