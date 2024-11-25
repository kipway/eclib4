/*!
\file ec_tcpclient.h
\author	jiangyong
\email  kipway@outlook.com
\update
  2024.6.7  sock5代理也改为异步发送,增加多客户端netio事件监听. 
  2024.4.25 改为异步发送,自带发送缓冲管理.
  2023.8.10 add send timeout micro define

tcp_c
	a class for tcp client, support socks5 proxy, asynchronous connection

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include <stdint.h>
#include <thread>
#include <time.h>
#include "ec_string.h"
#include "ec_string.hpp"
#include "ec_netio.h"

#ifndef EC_TCP_CLIENT_SNDBUF_HEAPSIZE
#if defined(_MEM_TINY) // < 256M
#define EC_TCP_CLIENT_SNDBUF_HEAPSIZE (512 * 1024) // 512K heap size
#elif defined(_MEM_SML) // < 1G
#define EC_TCP_CLIENT_SNDBUF_HEAPSIZE (1024 * 1024) // 1M heap size
#else
#define EC_TCP_CLIENT_SNDBUF_HEAPSIZE (4 * 1024 * 1024) // 4M heap size
#endif
#endif

#ifndef EC_TCP_CLIENT_SNDBUF_BLKSIZE
#define EC_TCP_CLIENT_SNDBUF_BLKSIZE (1024 * 32)
#endif

#ifndef EC_TCP_CLIENT_SNDBUF_MAXSIZE
#if defined(_MEM_TINY) // < 256M
#define EC_TCP_CLIENT_SNDBUF_MAXSIZE (4 * 1024 * 1024) //4M
#elif defined(_MEM_SML) // < 1G
#define EC_TCP_CLIENT_SNDBUF_MAXSIZE (16 * 1024 * 1024) //16M
#else
#define EC_TCP_CLIENT_SNDBUF_MAXSIZE (128 * 1024 * 1024) //128M
#endif
#endif

namespace ec
{
	class tcp_c
	{
	public:
		enum st_sock {
			st_invalid = 0,
			st_connect = 1, // connecting...
			st_s5handshake = 2,  // socks5 handshake...
			st_s5request = 3,  // socks5 requesting...
			st_connected = 4,   // connected
			st_logined
		};
	protected:
		st_sock	_status;
		SOCKET _sock;
		char _sip[48]; //dest IP or socks5 proxy IP
		uint16_t _uport; //dest port or socks5 proxy port

		char _s5domain[40]; //socks5 domain parameter
		uint16_t _s5port; //socks5 port parameter

		int _timeover_connect_sec;//connect timeout seconds
	private:
		time_t _timeconnect; //start connect time.
		bytes _rs5buf; // buf for socks5 request
		bool _btcpnodelay;  // default false
		bool _btcpkeepalive; // default true
		ec::blk_alloctor<> _sndbufblks; //发送缓冲分配区
		ec::io_buffer<> _sndbuf;//发送缓冲
		uint8_t _rbuf[1024 * 20];
	public:
		tcp_c() : _status(st_invalid)
			, _sock(INVALID_SOCKET)
			, _sip{ 0 }
			, _uport(0)
			, _s5domain{ 0 }
			, _s5port(0)
			, _btcpnodelay(false)
			, _btcpkeepalive(true)
			, _sndbufblks(EC_TCP_CLIENT_SNDBUF_BLKSIZE - EC_ALLOCTOR_ALIGN, EC_TCP_CLIENT_SNDBUF_HEAPSIZE / EC_TCP_CLIENT_SNDBUF_BLKSIZE)
			, _sndbuf(EC_TCP_CLIENT_SNDBUF_MAXSIZE, &_sndbufblks)
		{
			_timeconnect = ::time(nullptr);
			_timeover_connect_sec = 8;
			_rs5buf.reserve(1000);
		}

		void set_tcp(bool bnodelay, bool bkeepalive) //call before  open
		{
			_btcpnodelay = bnodelay;
			_btcpkeepalive = bkeepalive;
		}

		virtual ~tcp_c()
		{
			if (INVALID_SOCKET != _sock) {
				::closesocket(_sock);
				_sock = INVALID_SOCKET;
			}
			_status = st_invalid;
		}
		/*!
		\brief connect asynchronous , support socks5 proxy
		\param sip dest IP or socks5 proxy IP
		\param uport dest port or socks5 proxy port
		\param timeoverseconds timeout seconds
		\param sdomain socks5 domain parameter，nullptr not use socks5,domain format "google.com"
		\param s5portsocks5 port parameter, 0 not use socks5
		*/
		bool open(const char *sip, uint16_t uport, int timeoverseconds = 8, const char* sdomain = nullptr, uint16_t s5port = 0)
		{
			if (INVALID_SOCKET != _sock)
				return true;

			_timeover_connect_sec = timeoverseconds;
			_timeconnect = ::time(nullptr);

			if (!sip || !*sip || !uport)
				return false;

			ec::strlcpy(_sip, sip, sizeof(_sip));
			_uport = uport;

			ec::strlcpy(_s5domain, sdomain, sizeof(_s5domain));
			_s5port = s5port;

			int st = -1;
			_sock = net::tcpconnectasyn(_sip, _uport, st);
			if (INVALID_SOCKET == _sock)
				return false;

			_rs5buf.clear();
			_rs5buf.reserve(1000);
			if (st) { //connecting
#ifdef _WIN32
				if (WSAEWOULDBLOCK != WSAGetLastError()) {
					::closesocket(_sock);
					_sock = INVALID_SOCKET;
					connectfailed();
					return false;
				}
#else
				if (EINPROGRESS != errno) {
					::closesocket(_sock);
					_sock = INVALID_SOCKET;
					connectfailed();
					return false;
				}
#endif
				_status = st_connect;//connecting...
				return true;
			}

			if (_s5domain[0] && _s5port) {
				if (!sendsock5handshake()) {
					connectfailed();
					return false;
				}
				_status = st_s5handshake;
				return true;
			}

			_status = st_connected;
			onconnected();
			return true;
		}

		void close(int notify = 1)
		{
			if (INVALID_SOCKET != _sock) {
				::closesocket(_sock);
				_sock = INVALID_SOCKET;
				_status = st_invalid;
				if(notify)
					ondisconnected();
			}
		}

		/**
		 * @brief send synchronously 
		 * @param p data
		 * @param nlen datasize
		 * @return the number of bytes sent; -1: error and close connection;
		 * 2024-4-25改为异步发送
		*/
		virtual int sendbytes(const void* p, int nlen)
		{
			if (INVALID_SOCKET == _sock || _status < st_connected)
				return -1;
			if(AsyncSend(p, nlen) < 0){
				close();
				return -1;
			}
			return nlen;
		}

		inline int get_tcp_status()
		{
			return _status;
		}

	protected:
		virtual void onconnected()
		{
			if (_btcpnodelay)
				ec::net::tcpnodelay(_sock);
			if (_btcpkeepalive)
				ec::net::setkeepalive(_sock);
		}

		virtual void onconnectfailed()
		{
		}

		virtual void ondisconnected()
		{
		}

		virtual void onreadbytes(const uint8_t* p, int nbytes) = 0;

		virtual void onidle()
		{
		}
	public:
		/**
		 * @brief 获取poll参数,用于多个客户端异步IO
		 * @return  -1:不可用； 0：success
		 */
		int getPoll(pollfd& outpoll) {
			if (INVALID_SOCKET == _sock) {
				outpoll.revents = 0;
				return -1;
			}
			outpoll.fd = _sock;
			outpoll.events = 0;
			outpoll.revents = 0;

			if (st_connect == _status) { //连接中
				outpoll.events = POLLOUT;
			}
			else {
				outpoll.events = POLLIN;
				if (!_sndbuf.empty())
					outpoll.events |= POLLOUT;
			}
			return 0;
		}

		static void getPollString(int evt, std::string& sout)
		{
			struct t_i {
				int evt;
				const char* s;
			};
			static t_i estr[]{
				{POLLIN,"POLLIN"},
				{POLLOUT,"POLLOUT"},
				{POLLERR,"POLLERR"},
				{POLLHUP,"POLLHUP"},
				{POLLNVAL,"POLLNVAL"}
			};
			int n = 0;
			sout.clear();
			for (auto i = 0u; i < (sizeof(estr) / sizeof(t_i)); i++) {
				if (evt & estr[i].evt) {
					if (n)
						sout.push_back('|');
					sout.append(estr[i].s);
					++n;
				}
			}
		}

		/**
		 * @brief 多客户端复用
		 * @param revents poll事件
		 */
		void runtimePoll(int revents) 
		{
			if (st_connect == _status)
				doconnect(revents);
			else if (st_s5handshake == _status)
				dosocks5handshake(revents);
			else if (st_s5request == _status)
				dosocks5request(revents);
			else  if (_status >= st_connected)
				doNetIO(revents);
			onidle();
		}

		/*!
		\brief run time,独享线程时使用
		\param nmsec waitIO event millisecond(1/1000 second)
		*/
		void runtime(int nmsec)
		{
			pollfd fdpoll{ INVALID_SOCKET, 0, 0 };
			if (getPoll(fdpoll) < 0) {
				if(nmsec > 0)
					std::this_thread::sleep_for(std::chrono::milliseconds(nmsec));
				runtimePoll(0);
				return;
			}
			int n = 0;
#ifdef _WIN32
			n = WSAPoll(&fdpoll, 1, nmsec);
#else
			n = poll(&fdpoll, 1, nmsec);
#endif
			if(n >= 0)
				runtimePoll(fdpoll.revents);
		}

	private:
		/**
		 * @brief 异步发送，失败会关闭连接
		 * @param pbuf 
		 * @param nsize 
		 * @return -1:失败; >=0实际发送的字节数
		 */
		int AsyncSend(const void* pbuf, int nsize)
		{
			int ns = 0;
			const char* pc = (const char*)pbuf;
			if (_sndbuf.empty()) { //空先发送，剩余的加入缓冲
				ns = net::_send_non_block(_sock, pc, nsize);
				if (ns < 0 || (ns < nsize && !_sndbuf.append(pc + ns, nsize - ns))) {
					return -1;
				}
			}
			else { //非空，先追加到缓冲, 再发送
				if (!_sndbuf.append(pc, nsize)) { // full
					return -1;
				}
				ns = sendbuf_();
			}
			return ns;
		}

		void connectfailed()
		{
			if (INVALID_SOCKET != _sock) {
				::closesocket(_sock);
				_sock = INVALID_SOCKET;
			}
			_status = st_invalid;
			onconnectfailed();
		}

		bool sendsock5handshake()
		{
			uint8_t frm[4];
			frm[0] = 0x05;//VER
			frm[1] = 0x01;//NMETHODS
			frm[2] = 0x00;//NO AUTHENTICATION REQUIRED
			return AsyncSend(frm, 3) >= 0;
		}

		bool sendsock5request()
		{
			uint8_t frm[256], ul = (uint8_t)strlen(_s5domain);
			frm[0] = 0x05;//ver
			frm[1] = 0x01;//connect
			frm[2] = 0x00;//res
			frm[3] = 0x03;//domain
			frm[4] = ul;
			memcpy(&frm[5], _s5domain, ul);
			frm[5 + ul] = (uint8_t)((_s5port & 0xff00) >> 8);
			frm[5 + ul + 1] = (uint8_t)(_s5port & 0xff);
			return AsyncSend(frm, 7 + ul) >= 0;
		}

		void dosocks5handshake(int revents)
		{
			if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
				connectfailed();
				return;
			}
			if (revents & POLLOUT) {
				if (sendbuf_() < 0) {
					connectfailed();
					return;
				}
			}
			if (revents & POLLIN) {
				uint8_t sbuf[256];
				int nr = ::recv(_sock, (char*)sbuf, (int)sizeof(sbuf), 0);
				if (nr == 0) { // close
					connectfailed();
					return;
				}
				else if (nr < 0) {
#ifdef _WIN32
					if (WSAEWOULDBLOCK != (int)WSAGetLastError()) {
#else
					if (EAGAIN != errno && EWOULDBLOCK != errno) {
#endif
						connectfailed();
						return;
					}
				}
				else {
					_rs5buf.append(sbuf, nr);
					if (_rs5buf.size() < 2)
						return;
					if (_rs5buf[0] != 5u || _rs5buf[1] != 0) { // handshake failed
						connectfailed();
						return;
					}
					_rs5buf.clear();
					if (!sendsock5request()) {
						connectfailed();
						return;
					}
					_status = st_s5request;
				}
			}
			else {
				if (::time(nullptr) - _timeconnect > _timeover_connect_sec) { //time over
					connectfailed();
					return;
				}
			}
		}

		void dosocks5request(int revents)
		{
			if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
				connectfailed();
				return;
			}
			if (revents & POLLOUT) {
				if (sendbuf_() < 0) {
					connectfailed();
					return;
				}
			}
			if (revents & POLLIN) {
				uint8_t sbuf[256];
				int nr = ::recv(_sock, (char*)sbuf, (int)sizeof(sbuf), 0);
				if (nr == 0) { // close
					connectfailed();
					return;
				}
				else if (nr < 0) {
#ifdef _WIN32
					if (WSAEWOULDBLOCK != (int)WSAGetLastError()) {

#else
					if (EAGAIN != errno && EWOULDBLOCK != errno) {
#endif
						connectfailed();
						return;
					}
				}
				else {
					_rs5buf.append(sbuf, nr);
					if (_rs5buf.size() < 7)
						return;
					if (_rs5buf[0] != 5u || _rs5buf[1] != 0) {
						connectfailed();
						return;
					}

					uint8_t ul = _rs5buf[4];
					if (_rs5buf.size() >= 7u + ul) {
						_rs5buf.erase(0, ul + 7u);
						_status = st_connected;
						onconnected();
						if (_rs5buf.size()) {
							onreadbytes(_rs5buf.data(), (int)_rs5buf.size());
							_rs5buf.clear();
							_rs5buf.shrink_to_fit();
						}
					}
				}
			}
			else {
				if (::time(nullptr) - _timeconnect > _timeover_connect_sec) { //time over
					connectfailed();
					return;
				}
			}
		}

		int sendbuf_()
		{
			int nsall = 0, ns;
			size_t zlen = 0;
			const char* pc = (const char*)_sndbuf.get(zlen);
			while (pc && zlen) {
				ns = net::_send_non_block(_sock, pc, (int)zlen);
				if (ns < 0) {
					close();
					return -1;
				}
				else if (0 == ns)
					break;
				nsall += ns;
				_sndbuf.freesize(ns);
				pc = (const char*)_sndbuf.get(zlen);
			}
			return nsall;
		}

		void doNetIO(int revents)
		{
			if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
				close();
				return;
			}
			if (revents & POLLOUT) {
				if (sendbuf_() < 0) {
					close();
					return;
				}
			}
			if (revents & POLLIN) {
				int nmax = 8;
				while (nmax && _status >= st_connected) {
					--nmax;
					int nr = ::recv(_sock, (char*)_rbuf, (int)sizeof(_rbuf), 0);
					if (nr == 0) { // close
						close();
						return;
					}
					else if (nr < 0) {
#ifdef _WIN32
						if (WSAEWOULDBLOCK != WSAGetLastError()) {
#else
						if (EAGAIN != errno && EWOULDBLOCK != errno) {
#endif
							close();
						}
						return;
					}
					else {
						onreadbytes(_rbuf, nr); //应用层可调用close主动断开, _status值改变会终止循环。
					}
				}
			}
		}

		void doconnect(int revents)
		{
			if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
				connectfailed();
				return;
			}
			else if (revents & POLLOUT) {
#ifndef _WIN32
				int serr = 0;
				socklen_t serrlen = sizeof(serr);
				getsockopt(_sock, SOL_SOCKET, SO_ERROR, (void*)&serr, &serrlen);
				if (serr) {
					connectfailed();
					return;
				}
#endif
				if (_s5domain[0] && _uport) { //走代理
					if (!sendsock5handshake()) {
						connectfailed();
						return;
					}
					_status = st_s5handshake;
					return;
				}
				_status = st_connected;
				onconnected();
			}
			else {
				if (::time(nullptr) - _timeconnect > _timeover_connect_sec) //timeover
					connectfailed();
			}
		}
	};
}