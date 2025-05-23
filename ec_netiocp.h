﻿/*
* @file ec_netiocp.h
* base net server class use IOCP for windows
* @author jiangyong
* @update
	2024-12-30 优化udp发送
	2024.11.9 support no ec_alloctor
	2024-7-28 修复oniocp_accept未判断nextfd返回-1满链接场景
	2024-5-8 增加主动断开处理，用于发送websocket断开控制帧。
	2024-4-29 删除windows网络库初始化，移到ec::CServerApp中
	2024-4-13 增加onSendBufSizeChanged()
	2024-4-9 消息处理后发送触发移到平台相关层.
	2024-4-3 更新closefd
	2023-12-21 增加总收发流量和总收发秒流量
	2023-6-15 add tcp keepalive
	2023-6-6  增加可持续fd, update closefd() 可选通知
    2023-5-21 for http download big file

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include "ec_string.hpp"
#include "ec_diskio.h"
#include "ec_log.h"
#include "ec_map.h"
#include "ec_aiosession.h"
#include "ec_vector.hpp"

#ifndef SIZE_MAX_FD
#define SIZE_MAX_FD  16384
#endif

#ifndef SIZE_IOCP_POSTSNDBUF
#define SIZE_IOCP_POSTSNDBUF (EC_AIO_SNDBUF_BLOCKSIZE - EC_ALLOCTOR_ALIGN) //size post send buffer
#endif

#ifndef SIZE_IOCP_POSTRCVBUF
#define SIZE_IOCP_POSTRCVBUF EC_AIO_READONCE_SIZE //size post recv buffer
#endif

#ifndef EC_AIO_UDP_MAXSIZE
#define EC_AIO_UDP_MAXSIZE 65507
#endif

#ifndef EC_AIO_UDP_NUMOVL
#define EC_AIO_UDP_NUMOVL  8
#endif
#ifndef FRMS_UDP_SEND_ONCE
#define FRMS_UDP_SEND_ONCE 8
#endif
namespace ec {
	namespace aio {
		class serveriocp_
		{
		public:
			enum fdtype {
				fd_tcp = 0,
				fd_tcpout,
				fd_listen,
				fd_udp,
				fd_iocp
			};

			enum op_type {
				op_nul = 0,//unkown
				op_accept,//accept completed
				op_recv, // recv completed
				op_send, // send completed
				op_recvfrom, // udp recvfrom completed
				op_sendto, // udp sendto completed
				op_sendreq // self define send request
			};

			struct t_overlap
			{
				WSAOVERLAPPED overlap;
				WSABUF wsbuf;
				int kfd; // fdlisten if optype == op_accept
				int optype;
				SOCKET sysfd; //accept socket if optype == op_accept
				size_t wsbufsize;
				union
				{
					struct t_inaddr {
						struct sockaddr_in6 _netaddr;
						int _addrlen;
						t_inaddr() {
							clear();
						}
						void clear() {
							_addrlen = 0;
							memset(&_netaddr, 0, sizeof(_netaddr));
						}
						inline const struct sockaddr* addr() const {
							return (const struct sockaddr*)&_netaddr;
						}
						inline int addrlen() const {
							return _addrlen;
						}
						void set(const struct sockaddr* paddr, size_t addrsize)
						{
							if (addrsize > 0 && addrsize <= (int)sizeof(_netaddr)) {
								memcpy(&_netaddr, paddr, addrsize);
								_addrlen = (int)addrsize;
							}
						}
						inline struct sockaddr* buffer() {
							return (struct sockaddr*)&_netaddr;
						}
						inline int* psize()
						{
							_addrlen = sizeof(_netaddr);
							return &_addrlen;
						}
					}inaddr; //udp in addr information
					char acceptbuf[200];//buffer for accept
				};
			};

			struct t_fd
			{
				int  kfd; //>=0;  <0 error; key, key fd
				int  fdtype;
				SOCKET sysfd; // system fd
				int  sa_family; // AF_INET or AF_INET6
				int  post_snd;// send post uncompleted
				int  post_rcv;// recv post uncompleted
			};

			struct keq_fd
			{
				bool operator()(int key, const t_fd& val)
				{
					return key == val.kfd;
				}
			};

			static const char* optype2str(int op) {
				static const char* s[] = { "nul","accept","recv","send","recvfrom","sendto","sendreq" };
				if (op < 0 || op >= (int)sizeof(s) / sizeof(char*)) {
					return "unkown";
				}
				return s[op];
			}
		protected:
			ec::ilog* _plog;

			HANDLE _hiocp;
			ec::hashmap<int, t_fd, keq_fd> _mapfd;
			int _nextfd; // from 1-INT32_MAX
			ec::string _sfdfile;

			int nextfd()
			{
				if (_mapfd.size() >= SIZE_MAX_FD)
					return -1;
				do {
					++_nextfd;
					if (_nextfd == INT32_MAX)
						_nextfd = 1;
				} while (_mapfd.has(_nextfd));
				if (!_sfdfile.empty()) {
					FILE* pf = ec::io::fopen(_sfdfile.c_str(), "wt");
					if (pf) {
						char sid[40] = { 0 };
						snprintf(sid, sizeof(sid), "%d", _nextfd);
						fwrite(sid, 1, strlen(sid), pf);
						fclose(pf);
					}
				}
				return _nextfd;
			}
			
		public:
			void SetFdFile(const char* sfile)
			{
				if (!sfile || !*sfile) {
					_sfdfile.reserve(200);
					ec::io::getexepath(_sfdfile);
					_sfdfile.append("EcNetVfd.txt");
				}
				else
					_sfdfile = sfile;
				FILE* pf = ec::io::fopen(_sfdfile.c_str(), "rt");
				if (pf) {
					char sid[40] = { 0 };
					fread(sid, 1, sizeof(sid) - 1u, pf);
					fclose(pf);
					_nextfd = atoi(sid);
					if (_nextfd < 0)
						_nextfd = 0;
				}
			}
			inline bool hasfd(int kfd)
			{
				return _mapfd.has(kfd);
			}
		protected:
			/**
			 * @brief 主动断开,用于特殊处理websocket断开发送握手信号
			 * @param kfd
			*/
			virtual void onCloseFd(int kfd) = 0;

			/**
			 * @brief before disconnect call
			 * @param kfd keyfd
			*/
			virtual void onDisconnect(int kfd) = 0;

			/**
			 * @brief after disconnect call
			 * @param kfd keyfd
			*/
			virtual void onDisconnected(int kfd) = 0;

			/**
			 * @brief received data
			 * @param kfd keyfd
			 * @param pdata Received data
			 * @param size  Received data size
			 * @return 0:OK; -1:error
			*/
			virtual int onReceived(int kfd, const void* pdata, size_t size) = 0;

			/**
			 * @brief received UDP data
			 * @param kfd keyfd
			 * @param pdata Received data
			 * @param size  Received data size
			 * @param addrfrom peer address
			 * @param size of peer address
			 * @return 0:OK; -1:error
			*/
			virtual int onReceivedFrom(int kfd, const void* pdata, size_t size, const struct sockaddr* addrfrom, int addrlen) {
				return 0;
			}

			/**
			 * @brief TCP Accept
			 * @param kfd keyfd
			 * @param sip peer ip
			 * @param port peer port
			 * @param kfd_listen keyfd of listened
			*/
			virtual void onAccept(int kfd, const char* sip, uint16_t port, int kfd_listen) = 0;

			/**
			 * @brief size can receive ,use for flowctrl
			 * @param pss
			 * @return >0 size can receive;  0: pause read
			*/
			virtual size_t  sizeCanRecv(psession pss) {
				return SIZE_IOCP_POSTRCVBUF;
			};

			/**
			* @brief TCP asyn connect out success
			* @param kfd keyfd
			* @remark will call onDisconnect and onDisconnected if failed.
			*/
			virtual void onTcpOutConnected(int kfd) {
			}

			/**
			 * @brief get the session of kfd
			 * @param kfd keyfd
			 * @return nullptr or psession
			*/
			virtual psession getSession(int kfd) = 0;

			virtual void onSendtoFailed(int kfd, const struct sockaddr* paddr, int addrlen, const void* pdata, size_t datasize, int errcode) {};
			virtual void onSendCompleted(int kfd, size_t size) {};
			virtual void onSendBufSizeChanged(int kfd, size_t sendbufsize, int protocol) {};
		protected:
			int setsendbuf(int fd, int n)
			{
				int nval = n;
				t_fd* p = _mapfd.get(fd);
				if (!p)
					return -1;
				if (-1 == setsockopt(p->sysfd, SOL_SOCKET, SO_SNDBUF, (char*)&nval, (socklen_t)sizeof(nval)))
					return -1;
				return nval;
			}

			int setrecvbuf(int fd, int n)
			{
				int nval = n;
				t_fd* p = _mapfd.get(fd);
				if (!p)
					return -1;
				if (-1 == setsockopt(p->sysfd, SOL_SOCKET, SO_RCVBUF, (char*)&nval, (socklen_t)sizeof(nval)))
					return -1;
				return nval;
			}

			int getbufsize(int fd, int op)
			{
				int nlen = 4, nval = 0;
				t_fd* p = _mapfd.get(fd);
				if (!p)
					return -1;
				if (getsockopt(p->sysfd, SOL_SOCKET, op, (char*)&nval, (socklen_t*)&nlen) < 0)
					return -1;
				return nval;
			}

			/**
			 * @brief asyn connect out
			 * @param addr
			 * @param addrlen
			 * @return keyfd; -1:failed;
			*/
			int connect_asyn(const struct sockaddr* addr, socklen_t addrlen) //connect nobloack, return fd
			{
				t_fd t{ 0,0,0,0,0 };
				t.kfd = nextfd();
				if (t.kfd < 0)
					return -1;
				t.sa_family = addr->sa_family;
				t.sysfd = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
				if (INVALID_SOCKET == t.sysfd)
					return -1;
				long ul = 1;
				if (SOCKET_ERROR == ioctlsocket(t.sysfd, FIONBIO, (unsigned long*)&ul)) {
					closesocket(t.sysfd);
					return -1;
				}
				connect(t.sysfd, addr, addrlen);
				t.fdtype = fd_tcpout;
				_mapfd.set(t.kfd, t);
				return t.kfd;
			}

			/**
			 * @brief shutdown and close a kfd
			 * @param kfd  keyfd
			 * @return -1: kfd not exist; 0:success
			*/
			int close_(int kfd) //shutdown and close kfd
			{
				t_fd* p = _mapfd.get(kfd);
				if (!p)
					return -1;
				_plog->add(CLOG_DEFAULT_DBG, "close_ fd(%d), fdtype = %d, socket = %u", kfd, p->fdtype, p->sysfd);
				if (fd_tcp == p->fdtype || fd_tcpout == p->fdtype)
					shutdown(p->sysfd, SD_BOTH);
				closesocket(p->sysfd);
				_mapfd.erase(kfd);
				return 0;
			}

			bool setkeepalive(int kfd, bool bfast = false)
			{
				t_fd* p = _mapfd.get(kfd);
				if (!p)
					return false;

				BOOL bKeepAlive = 1;
				int nRet = setsockopt(p->sysfd, SOL_SOCKET, SO_KEEPALIVE,
					(char*)&bKeepAlive, sizeof(bKeepAlive));
				if (nRet == SOCKET_ERROR)
					return false;
				tcp_keepalive alive_in;
				tcp_keepalive alive_out;
				if (bfast) {
					alive_in.keepalivetime = 5 * 1000;
					alive_in.keepaliveinterval = 1000;
				}
				else {
					alive_in.keepalivetime = 30 * 1000;
					alive_in.keepaliveinterval = 5000;
				}
				alive_in.onoff = 1;
				unsigned long ulBytesReturn = 0;

				nRet = WSAIoctl(p->sysfd, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in),
					&alive_out, sizeof(alive_out), &ulBytesReturn, NULL, NULL);
				if (nRet == SOCKET_ERROR)
					return false;
				return true;
			}

		public:
			serveriocp_(ec::ilog* plog) : _plog(plog), _hiocp(nullptr), _nextfd(0)
			{
			}
			virtual ~serveriocp_() {
			}
			/**
			 * @brief create IOCP handel
			 * @return 0:ok; -1:error
			*/
			int open(const char* spre = nullptr)
			{
				if (nullptr != _hiocp)
					return 0;

				_hiocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)0, 0);
				if (nullptr == _hiocp) {
					_plog->add(CLOG_DEFAULT_ERR, "%sCreateIoCompletionPort failed with error: %u", spre ? spre : "", GetLastError());
					return -1;
				}
				_plog->add(CLOG_DEFAULT_INF, "%sCreateIoCompletionPort success.", spre ? spre : "");
				return 0;
			}

			/**
			 * @brief 关闭所有连接和IOCP handel,释放接收和发送投递缓冲
			 * @return 0;
			 * @remark 用于退出时调用，不会通知应用层连接断开，应用层需自己释放和连接相关的资源。
			*/
			int close()
			{
				for (auto& i : _mapfd) {//先关闭所有socket
					if (fd_tcp == i.fdtype || fd_tcpout == i.fdtype || fd_udp == i.fdtype)
						shutdown(i.sysfd, SD_BOTH);
					closesocket(i.sysfd);
					_plog->add(CLOG_DEFAULT_DBG, "close fd(%d), fdtype = %d, socket = %u @serveriocp_::close",
						i.kfd, i.fdtype, i.sysfd);
				}
				_mapfd.clear();
				while (runtime_(0) > 0);
				if (nullptr != _hiocp) {
					CloseHandle(_hiocp);
					_hiocp = nullptr;
				}
				return 0;
			}

			/**
			 * @brief tcp listen
			 * @param port port
			 * @param sip  ipv4 or ipv6, nullptr or empty is ipv4 0.0.0.0
			 * @return virtual fd; -1:failed
			*/
			int tcplisten(uint16_t port, const char* sip = nullptr, int ipv6only = 0)
			{
				ec::net::socketaddr netaddr;
				if (netaddr.set(port, sip) < 0)
					return -1;
				int addrlen = 0;
				struct sockaddr* paddr = netaddr.getsockaddr(&addrlen);
				if (!paddr)
					return -1;
				t_fd tf{ 0, 0, INVALID_SOCKET, 0, 0 ,0 };
				tf.sa_family = netaddr.sa_family();
				int fdl = bind_listen(paddr, addrlen, tf, ipv6only);
				if (fdl < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "bind listen tcp://%s:%u failed.", netaddr.viewip(), port);
					return -1;
				}
				if (!CreateIoCompletionPort((HANDLE)tf.sysfd, _hiocp, (ULONG_PTR)fdl, 0)) {
					_plog->add(CLOG_DEFAULT_ERR, "CreateIoCompletionPort failed with error: %u ", GetLastError());
					close_(fdl);
					return -1;
				}
				if (-1 == accept_(fdl)) {
					close_(fdl);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_MSG, "fd(%d) bind listen tcp://%s:%u success.", fdl, netaddr.viewip(), port);
				return fdl;
			}

			int udplisten(uint16_t port, const char* sip = nullptr, int ipv6only = 0) // return udp server fd, -1 error
			{
				ec::net::socketaddr netaddr;
				if (netaddr.set(port, sip) < 0)
					return -1;
				int addrlen = 0;
				struct sockaddr* paddr = netaddr.getsockaddr(&addrlen);
				if (!paddr)
					return -1;
				t_fd tf{ 0,0,INVALID_SOCKET,0,0, 0 };
				tf.sa_family = netaddr.sa_family();
				int fdl = create_udp(paddr, addrlen, tf, ipv6only);

				if (fdl < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "bind udp %s:%u failed.", netaddr.viewip(), port);
					return -1;
				}

				if (!CreateIoCompletionPort((HANDLE)tf.sysfd, _hiocp, (ULONG_PTR)tf.kfd, 0)) {
					_plog->add(CLOG_DEFAULT_ERR, "udp listen fd(%d), socket(%u) CreateIoCompletionPort failed with error: %u ",
						tf.kfd, tf.sysfd, GetLastError());
					close_(tf.kfd);
					return -1;
				}

				if (postreadfrom_(tf.kfd) < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "fd(%d) postreadfrom_ failed", tf.kfd);
					close_(tf.kfd);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_MSG, "fd(%d) bind udp://%s:%u success.", fdl, netaddr.viewip(), port);
				return fdl;
			}

			/**
			 * @brief 运行时
			 * @param nmsec
			 * @return 0:没有消息，1：处理一次； -1：IOCP关闭。
			*/
			int runtime_(int waitmsec)
			{
				if (nullptr == _hiocp)
					return -1;

				int64_t curmstime = ec::mstime();
				if (llabs(curmstime - _lastmstime) >= 4) { //4毫秒任务
					dorecvflowctrl();//流控处理
					doaysnconnectout();//连出处理
					_lastmstime = curmstime;
				}

				DWORD			dwBytes = 0;
				LPOVERLAPPED	pOverlapped = nullptr;
				ULONG_PTR		uKey = 0; // fdl, 虚拟fd， CreateIoCompletionPort中的 CompletionKey

				BOOL rc = GetQueuedCompletionStatus(
					_hiocp,
					&dwBytes,
					&uKey,
					(LPOVERLAPPED*)&pOverlapped,
					waitmsec);

				if (!rc) { //失败的完成消息
					t_overlap* pol = (t_overlap*)pOverlapped;
					return oniocp_ststausfalse(pol);
				}
				//成功的完成消息
				t_overlap* pol = (t_overlap*)pOverlapped;
				if (!pol) { //自定义的触发fd发送消息
					int kfd = (int)uKey;
					t_fd* ptf = _mapfd.get(kfd);
					if (ptf && ptf->fdtype == fd_udp && ptf->post_snd < EC_AIO_UDP_NUMOVL)
						postsendto_(kfd);
					else
						postsend_(kfd);
					return 1;
				}

				int nr = 0;
				switch (pol->optype) {
				case op_accept:
					nr = oniocp_accept(pol);
					break;
				case op_recv:
					nr = oniocp_recv(pol, dwBytes);
					break;
				case op_send:
					nr = oniocp_send(pol, dwBytes);
					break;
				case op_recvfrom:
					nr = oniocp_recvfrom(pol, dwBytes);
					break;
				case op_sendto:
					nr = oniocp_sendto(pol, dwBytes);
					break;

				case op_sendreq: //没有用
					if (1) {
						t_fd* ptf = _mapfd.get(pol->kfd);
						if (ptf && ptf->fdtype == fd_udp)
							postsendto_(pol->kfd);
						else
							postsend_(pol->kfd);
					}
					break;
				}
				freeoverlap(pol);
				return 1;
			}

			/**
			 * @brief put a send post request to event queue
			 * @param kfd keyfd
			 * @remark 如果UDP主动发送，使用这个函数触发UDP的消息投递机制
			*/
			void sendtrigger(int kfd, int num = 1)
			{
				if (_hiocp) {
					t_fd* ptf = _mapfd.get(kfd);
					if (ptf && ptf->post_snd < num)
						PostQueuedCompletionStatus(_hiocp, 0, (ULONG_PTR)kfd, nullptr);
				}
			}

			inline void udp_trigger(int kfd, bool bsend)
			{
				if (bsend)
					postsendto_(kfd);
				else
					sendtrigger(kfd);
			}

			/**
			 * @brief 如果缓冲不空，且没有投递，则投递一包
			 * @param kfd keyfd
			 * @return  >=0: post bytes;  -1:failed, close fd and call onDisconnected(kfd)
			*/
			inline int postsend(int kfd, int overlap = 0 )
			{
				return postsend_(kfd, overlap);
			}

			/**
			 * @brief 关闭连接，会产生onDisconnect和onDisconnected调用
			 * @param kfd  keyfd
			 * @param errorcode 网络错误码，0表示不是网络错误, 是服务端主动断开.
			 * @return 0:关闭; -1:不存在，之前已经被关闭.
			*/
			int closefd(int kfd, int errorcode)
			{
				if (!hasfd(kfd))
					return -1;
				onDisconnect(kfd);
				if (!errorcode) {
					onCloseFd(kfd);
				}
				close_(kfd);
				onDisconnected(kfd);
				return 0;
			}

			size_t size_fds()
			{
				return _mapfd.size();
			}

		private:
			int64_t _lastmstime = 0;//上次扫描可发送的时间，单位GMT毫秒
			LPFN_ACCEPTEX _lpfnAcceptEx = nullptr;
			GUID _GuidAcceptEx = WSAID_ACCEPTEX;
			struct t_readflow {
				int fd;
				size_t size;
			};
			void dorecvflowctrl()//接收流控
			{
				t_readflow fl{ 0,0 };
				ec::vector<t_readflow> fds;
				fds.reserve(200);
				psession pss;
				for (auto& i : _mapfd) {
					if (i.post_rcv <= 0 && i.fdtype != fd_listen && i.fdtype != fd_iocp && i.fdtype != fd_udp) {
						pss = getSession(i.kfd);
						if (pss && (fl.size = sizeCanRecv(pss)) > 0 && !pss->_readpause
							&& pss->_status >= EC_AIO_FD_CONNECTED) {
							fl.fd = i.kfd;
							fds.push_back(fl);
						}
					}
				}
				for (auto& i : fds) {
					postread_(i.fd, i.size);
				}
			}

			void doaysnconnectout() //处理异步连出
			{
				ec::vector<int> fdsok;
				ec::vector<int> fdserr;
				fdsok.reserve(32);
				fdserr.reserve(32);
				psession pss;
				for (auto& i : _mapfd) {
					if (i.fdtype == fd_tcpout) {
						pss = getSession(i.kfd);
						if (EC_AIO_FD_CONNECTING == pss->_status) {
							TIMEVAL tv = { 0, 0 };
							int ne = 0;
							fd_set fdw, fde;
							FD_ZERO(&fdw);
							FD_ZERO(&fde);
							FD_SET(i.sysfd, &fdw);
							FD_SET(i.sysfd, &fde);
							ne = ::select(0, nullptr, &fdw, &fde, &tv);

							if (SOCKET_ERROR == ne || ((ne > 0) && FD_ISSET(i.sysfd, &fde)))
								fdserr.push_back(i.kfd);
							if (ne > 0 && FD_ISSET(i.sysfd, &fdw)) {
								fdsok.push_back(i.kfd);
								pss->_status = EC_AIO_FD_CONNECTED;
							}
						}
					}
				}
				for (auto i : fdsok) {
					t_fd* ptf = _mapfd.get(i);
					if (!ptf)
						continue;
					if (!CreateIoCompletionPort((HANDLE)ptf->sysfd, _hiocp, (ULONG_PTR)ptf->kfd, 0)) {
						_plog->add(CLOG_DEFAULT_ERR, "tcpout fd(%d), socket(%u) CreateIoCompletionPort failed with error: %u ",
							ptf->kfd, ptf->sysfd, GetLastError());
						closefd(ptf->kfd, 5); //linux错误码IO错误
						continue;
					}
					postread_(i);
					onTcpOutConnected(i);
				}
				for (auto i : fdserr) {
					_plog->add(CLOG_DEFAULT_INF, "fd(%d) tcp connect out failed.", i);
					closefd(i, 111); //linux错误码，拒绝连接
				}
			}

			t_overlap* newoverlap(size_t databufsize, int optype, int kfd = 0)
			{
				t_overlap* pol = (t_overlap*)ec::g_malloc(sizeof(t_overlap));
				if (!pol)
					return nullptr;
				memset(pol, 0, sizeof(t_overlap));
				pol->optype = optype;
				pol->kfd = kfd;
				if (!databufsize) {
#ifdef _DEBUG
					_plog->add(CLOG_DEFAULT_ALL, "fd(%d) new overlap post %s overlap. wsbufsize=%zu",
						pol->kfd, optype2str(pol->optype), pol->wsbufsize);
#endif
					return pol;
				}
				pol->wsbuf.buf = (char*)ec::g_malloc(databufsize, &pol->wsbufsize);
				if (!pol->wsbuf.buf) {
					ec::g_free(pol);
					return nullptr;
				}
#ifdef _DEBUG
				_plog->add(CLOG_DEFAULT_ALL, "fd(%d) new overlap post %s overlap. wsbufsize=%zu",
					pol->kfd, optype2str(pol->optype), pol->wsbufsize);
#endif
				return pol;
			}

			void freeoverlap(t_overlap* pol)
			{
#ifdef _DEBUG
				_plog->add(CLOG_DEFAULT_ALL, "fd(%d) free overlap post %s overlap. wsbufsize=%zu, wsbuf.len=%u",
					pol->kfd, optype2str(pol->optype), pol->wsbufsize, (uint32_t)pol->wsbuf.len);
#endif
				if (pol->wsbuf.buf) {
					ec::g_free(pol->wsbuf.buf);
					pol->wsbuf.buf = nullptr;
				}
				ec::g_free(pol);
			}

			int bind_listen(const struct sockaddr* addr, socklen_t addrlen, t_fd& tfout, int ipv6only = 0) // bind and listen, return fd
			{
				tfout.kfd = nextfd();
				if (tfout.kfd < 0)
					return -1;
				tfout.fdtype = fd_listen;
				tfout.sa_family = addr->sa_family;
				tfout.sysfd = WSASocket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
				if (INVALID_SOCKET == tfout.sysfd)
					return -1;
				int opt = 1;
				u_long iMode = 1;
				if (ipv6only && addr->sa_family == AF_INET6) {
					if (-1 == setsockopt(tfout.sysfd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&opt, sizeof(opt))) {
						closesocket(tfout.sysfd);
						return -1;
					}
				}
				opt = 1;
				if (SOCKET_ERROR == setsockopt(tfout.sysfd, SOL_SOCKET, SO_REUSEADDR,
					(const char*)&opt, sizeof(opt)) || SOCKET_ERROR == ioctlsocket(tfout.sysfd, FIONBIO, &iMode)) {
					closesocket(tfout.sysfd);
					return -1;
				}
				if (bind(tfout.sysfd, addr, addrlen) == SOCKET_ERROR) {
					closesocket(tfout.sysfd);
					return -1;
				}
				if (listen(tfout.sysfd, SOMAXCONN) == SOCKET_ERROR) {
					closesocket(tfout.sysfd);
					return -1;
				}
				_mapfd.set(tfout.kfd, tfout);
				return tfout.kfd;
			}

			/**
			 * @brief Post accept with AcceptEx
			 * @param kfdlisten 监听keyfd
			 * @return 0:success; -1:failed.
			*/
			int accept_(int kfdlisten)
			{
				t_fd* pfd;
				if (nullptr == (pfd = _mapfd.get(kfdlisten)))
					return -1;

				//Create an accepting socket and AcceptEx
				t_overlap* pol = newoverlap(0, op_accept, kfdlisten);
				if (!pol) {
					_plog->add(CLOG_DEFAULT_ERR, "newoverlap @  accept_(%d)", kfdlisten);
					return -1;
				}
				pol->sysfd = socket(pfd->sa_family, SOCK_STREAM, IPPROTO_TCP);
				if (INVALID_SOCKET == pol->sysfd) {
					_plog->add(CLOG_DEFAULT_ERR, "Create accept socket failed with error: %d", WSAGetLastError());
					freeoverlap(pol);
					return -1;
				}

				if (!AcceptEx(pfd->sysfd,
					pol->sysfd,
					pol->acceptbuf,
					0, // no data buffer
					sizeof(sockaddr_in6) + 16,// dwLocalAddressLength
					sizeof(sockaddr_in6) + 16,// dwRemoteAddressLength
					nullptr,
					(LPOVERLAPPED)pol)) {
					int werr = WSAGetLastError();
					if (ERROR_IO_PENDING != werr) {
						_plog->add(CLOG_DEFAULT_ERR, "AcceptEx failed with error: %d", werr);
						closesocket(pol->sysfd);
						freeoverlap(pol);
						return -1;
					}
				}
				return 0;
			}

			/**
			 * @brief post a send if post_snd == 0
			 * @param kfd  keyfd
			 * @return >=0: post bytes;  -1:failed, close fd and call onDisconnected(kfd)
			*/
			int postsend_(int kfd, int overlap = 0)
			{
				t_fd* pfd;
				if (nullptr == (pfd = _mapfd.get(kfd)))
					return -1;
				if (pfd->post_snd > overlap)
					return 0;
				psession pss = getSession(kfd);
				if (!pss)
					return 0;
				if (pss->_sndbuf.empty()) {
					if (!pss->onSendCompleted()) {
						if (_plog)
							_plog->add(CLOG_DEFAULT_MSG, "disconnect fd(%d) @onSendCompleted() failed", kfd);
						closefd(kfd, 0);//主动断开
						return -1;
					}
				}
				const void* pd = nullptr;
				size_t zlen = 0;
				pd = pss->_sndbuf.get(zlen);
				if (!pd || !zlen)
					return 0;
				if (zlen > SIZE_IOCP_POSTSNDBUF)
					zlen = SIZE_IOCP_POSTSNDBUF;

				t_overlap* pol = newoverlap(zlen, op_send, kfd);
				if (!pol)
					return -1;
				memcpy(pol->wsbuf.buf, pd, zlen);
				pol->wsbuf.len = (ULONG)zlen;

				int nret = WSASend(pfd->sysfd, &pol->wsbuf, 1, nullptr, 0, (LPWSAOVERLAPPED)pol, nullptr);
				if (SOCKET_ERROR == nret) {
					int nerr = WSAGetLastError();
					if (WSA_IO_PENDING != nerr) {
						_plog->add(CLOG_DEFAULT_ERR, "fd(%d) WSASend failed with error: %d.", kfd, nerr);
						closefd(kfd, 102);//linux错误码network dropped
						freeoverlap(pol);
						return -1;
					}
				}
				pfd->post_snd += 1;
				pss->_sndbuf.freesize(zlen);
				return (int)zlen;
			}

			/**
			 * @brief post a send if post_snd == 0
			 * @param kfd  keyfd
			 * @return >=0: post bytes;
			*/
			int postsendto_(int kfd)
			{
				int nsend = 0, nsndFrms = 0;
				do {
					t_fd* pfd;
					if (nullptr == (pfd = _mapfd.get(kfd)) || pfd->fdtype != fd_udp)
						return -1;
					if (pfd->post_snd >= EC_AIO_UDP_NUMOVL)
						break;

					psession pss = getSession(kfd);
					if (!pss)
						break;
					udb_buffer_* pfrms = pss->getudpsndbuffer();
					if (!pfrms || pfrms->empty())
						break;

					auto& frm = pfrms->front();
					t_overlap* pol = newoverlap(frm.size(), op_sendto, kfd);
					if (!pol)
						return -1;
					pol->inaddr.set(frm.getnetaddr(), frm.netaddrlen());
					pol->wsbuf.len = (ULONG)frm.size();
					memcpy(pol->wsbuf.buf, frm.data(), frm.size());
					pfrms->pop();
					int nret = WSASendTo(pfd->sysfd, &pol->wsbuf, 1, nullptr, 0,
						pol->inaddr.addr(), pol->inaddr.addrlen(), (LPWSAOVERLAPPED)pol, nullptr);
					if (SOCKET_ERROR == nret) {
						int nerr = WSAGetLastError();
						if (WSA_IO_PENDING != nerr) {
							onSendtoFailed(kfd, pol->inaddr.addr(), pol->inaddr.addrlen(), pol->wsbuf.buf, (size_t)pol->wsbuf.len, nerr);
							freeoverlap(pol);
						}
						break;
					}
					pfd->post_snd += 1;
					nsend += (int)pol->wsbuf.len;
					nsndFrms++;
				} while (nsndFrms < FRMS_UDP_SEND_ONCE && nsend < 1024 * 32);
				psession pss = getSession(kfd);
				if (pss)
					pss->onUdpSendCount(nsndFrms, nsend);
				return nsend;
			}

			/**
			 * @brief post a read if post_rcv == 0
			 * @param kfd keyfd
			 * @param sock
			 * @return 0：succeed； -1：failed, and closefd
			*/
			int postread_(int kfd, size_t size = SIZE_IOCP_POSTRCVBUF)
			{
				t_fd* ptf = _mapfd.get(kfd);
				if (!ptf)
					return -1;
				if (ptf->post_rcv > 0)
					return 0;

				DWORD dwFlag = 0;
				t_overlap* pol = newoverlap(size, op_recv, kfd);
				if (!pol)
					return -1;
				pol->wsbuf.len = (ULONG)size;
				int nret = WSARecv(ptf->sysfd, &pol->wsbuf, 1, nullptr, &dwFlag, (LPWSAOVERLAPPED)pol, nullptr);
				int nerr = 0;
				if (SOCKET_ERROR == nret && WSA_IO_PENDING != (nerr = WSAGetLastError())) {
					_plog->add(CLOG_DEFAULT_ERR, "fd(%d) WSARecv failed with error: %d.", kfd, nerr);
					closefd(kfd, 102);//linux错误码network dropped
					freeoverlap(pol);
					return -1;
				}
				ptf->post_rcv += 1;
				return 0;
			}

			/**
			 * @brief post a udp read if post_rcv == 0
			 * @param kfd keyfd
			 * @param sock
			 * @return 0：succeed； -1：failed, and closefd
			*/
			int postreadfrom_(int kfd)
			{
				int npost = 0;
				do {
					t_fd* ptf = _mapfd.get(kfd);
					if (!ptf || ptf->fdtype != fd_udp)
						return -1;
					if (ptf->post_rcv >= EC_AIO_UDP_NUMOVL + 2)
						return 0;

					DWORD dwFlag = 0;
					t_overlap* pol = newoverlap(EC_AIO_UDP_MAXSIZE, op_recvfrom, kfd);
					if (!pol)
						return -1;
					pol->wsbuf.len = EC_AIO_UDP_MAXSIZE;
					pol->inaddr.clear();
					int nret = WSARecvFrom(ptf->sysfd, &pol->wsbuf, 1, nullptr, &dwFlag,
						pol->inaddr.buffer(), pol->inaddr.psize(), (LPWSAOVERLAPPED)pol, nullptr);
					int nerr = 0;
					if (SOCKET_ERROR == nret && WSA_IO_PENDING != (nerr = WSAGetLastError())) {
						_plog->add(CLOG_DEFAULT_ERR, "fd(%d) WSARecvFrom failed with error: %d.", kfd, nerr);
						closefd(kfd, 102);//linux错误码network dropped
						freeoverlap(pol);
						return -1;
					}
					ptf->post_rcv += 1;
					npost = ptf->post_rcv;
				} while (npost < EC_AIO_UDP_NUMOVL + 2);
				return 0;
			}

			/**
			 * @brief 处理接收数据,根据流控决定是否继续投递读。
			 * @param pol
			 * @param dwBytes
			 * @return 0:success; -1:error and calls closefd
			*/
			int oniocp_recv(const t_overlap* pol, DWORD dwBytes) //读投递完成
			{
				if (!dwBytes) { //对端主动断开
					t_fd* pfd;
					if (nullptr != (pfd = _mapfd.get(pol->kfd))) {
						_plog->add(CLOG_DEFAULT_INF, "fd(%d) disconnected at op_read.", pol->kfd);
						closefd(pfd->kfd, 102);//linux错误码network dropped
					}
				}
				else {
#ifdef _DEBUG
					_plog->add(CLOG_DEFAULT_ALL, "fd(%d) recv %u bytes.", pol->kfd, dwBytes);
#endif
					if (onReceived(pol->kfd, pol->wsbuf.buf, dwBytes) < 0) {
						closefd(pol->kfd, 0);//主动断开
						return -1;
					}
					size_t sizeread = 0;
					t_fd* pfd;
					if (nullptr != (pfd = _mapfd.get(pol->kfd))) {
						if (pfd->post_rcv > 0)
							pfd->post_rcv -= 1;
						psession pss = getSession(pfd->kfd);
						if (pss && (sizeread = sizeCanRecv(pss)) > 0 && !pss->_readpause)
							postread_(pfd->kfd, sizeread);
						else {
							_plog->add(CLOG_DEFAULT_ALL, "fd(%d) pause reading for task balancing at windows CPIO",
								pfd->kfd);
						}
					}
					postsend_(pol->kfd);
				}
				return 0;
			}

			/**
			 * @brief 处理接收数据,根据流控决定是否继续投递读。
			 * @param pol
			 * @param dwBytes
			 * @return 0:success; -1:error and calls closefd
			*/
			int oniocp_recvfrom(const t_overlap* pol, DWORD dwBytes) //读投递完成
			{
				if (!dwBytes) {
					_plog->add(CLOG_DEFAULT_INF, "fd(%d) read 0bytes at op_readfrom. GetLastError:%u", pol->kfd, GetLastError());
				}
				else {
#ifdef _DEBUG
					_plog->add(CLOG_DEFAULT_ALL, "fd(%d) read %u bytes at op_readfrom.", pol->kfd, dwBytes);
#endif
					onReceivedFrom(pol->kfd, pol->wsbuf.buf, dwBytes, pol->inaddr.addr(), pol->inaddr.addrlen());
				}
				t_fd* pfd = _mapfd.get(pol->kfd);
				if (nullptr != pfd) {
					if (pfd->post_rcv > 0)
						pfd->post_rcv -= 1;
					postreadfrom_(pfd->kfd);
				}
				return 0;
			}

			int oniocp_send(const t_overlap* pol, DWORD dwBytes) // 发送投递完成
			{
				if (!dwBytes) { //对端主动断开
					t_fd* pfd;
					if (nullptr != (pfd = _mapfd.get(pol->kfd))) {
						_plog->add(CLOG_DEFAULT_INF, "fd(%d) disconnected at op_write.", pol->kfd);
						closefd(pfd->kfd, 102);//linux错误码network dropped
					}
					return 0;
				}
				psession pss = getSession(pol->kfd);
				if (pss) {
					pss->_allsend += dwBytes;
					pss->_bpsSnd.add(ec::mstime(), dwBytes);
					if (pss->_lastsndbufsize != pss->_sndbuf.size()) {
						pss->_lastsndbufsize = pss->_sndbuf.size();
						onSendBufSizeChanged(pol->kfd, pss->_lastsndbufsize, pss->_protocol);
					}
				}
				onSendCompleted(pol->kfd, (size_t)dwBytes);
#ifdef _DEBUG
				_plog->add(CLOG_DEFAULT_ALL, "fd(%d) op_write completed size %u.", pol->kfd, dwBytes);
#endif
				t_fd* pfd;
				if (nullptr != (pfd = _mapfd.get(pol->kfd))) {
					if (pfd->post_snd > 0)
						pfd->post_snd -= 1;
					postsend_(pol->kfd);
				}
				return 0;
			}

			int oniocp_sendto(const t_overlap* pol, DWORD dwBytes) // 发送投递完成
			{
				if (!dwBytes) { //对端主动断开
					t_fd* pfd;
					if (nullptr != (pfd = _mapfd.get(pol->kfd))) {
						_plog->add(CLOG_DEFAULT_INF, "fd(%d) disconnected at op_sendto.", pol->kfd);
						closefd(pfd->kfd, 102);//linux错误码network dropped
					}
					return 0;
				}
				onSendCompleted(pol->kfd, (size_t)dwBytes);
#ifdef _DEBUG
				_plog->add(CLOG_DEFAULT_ALL, "fd(%d) op_sendto completed size %u.", pol->kfd, dwBytes);
#endif
				t_fd* pfd;
				if (nullptr != (pfd = _mapfd.get(pol->kfd))) {
					if (pfd->post_snd > 0)
						pfd->post_snd -= 1;
					postsendto_(pol->kfd);
				}
				return 0;
			}

			int oniocp_accept(t_overlap* pol)
			{
				int len_l = 0, len_r = 0;
				sockaddr* addr_l = nullptr;
				sockaddr* addr_r = nullptr;
				GetAcceptExSockaddrs(pol->acceptbuf, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16,
					&addr_l, &len_l, &addr_r, &len_r);
				int seconds;
				int bytes = sizeof(seconds);
				int nret = 0;
				do {
					t_fd* ptfls = _mapfd.get(pol->kfd);// listenfd
					if (!ptfls || NO_ERROR != getsockopt(pol->sysfd, SOL_SOCKET, SO_CONNECT_TIME, (char*)&seconds, &bytes)) {
						_plog->add(CLOG_DEFAULT_ERR, "oniocp_accept getsockopt(SO_CONNECT_TIME) failed: %d", WSAGetLastError());
						closesocket(pol->sysfd);
						nret = -1;
						break;
					}

					t_fd tf;
					tf.kfd = nextfd();
					if (tf.kfd < 0) {
						_plog->add(CLOG_DEFAULT_ERR, "oniocp_accept nextfd failed, SIZE_MAX_FD = %d , session size = %zu",
							SIZE_MAX_FD, _mapfd.size());
						closesocket(pol->sysfd);
						return -1;
					}
					tf.fdtype = fd_tcp;
					tf.sysfd = pol->sysfd;
					tf.post_snd = 0;
					tf.post_rcv = 0;
					tf.sa_family = ptfls->sa_family;
					_mapfd.set(tf.kfd, tf);

					int iResult = 0;
					iResult = setsockopt(tf.sysfd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&ptfls->sysfd, sizeof(ptfls->sysfd));
					if (iResult) {
						_plog->add(CLOG_DEFAULT_ERR, "SO_UPDATE_ACCEPT_CONTEXT failed with error : %d", WSAGetLastError());
						close_(tf.kfd);
						nret = -1;
						break;
					}
					if (!CreateIoCompletionPort((HANDLE)tf.sysfd, _hiocp, (ULONG_PTR)tf.kfd, 0)) {
						_plog->add(CLOG_DEFAULT_ERR, "accept fd(%d), socket(%u) CreateIoCompletionPort failed with error: %u ",
							tf.kfd, tf.sysfd, GetLastError());
						close_(tf.kfd);
						nret = -1;
						break;
					}
					uint16_t uport = 0;
					char sipr[48] = { 0 };// remote ip
					if (addr_r->sa_family == AF_INET) {
						uport = ntohs(((struct  sockaddr_in*)addr_r)->sin_port);
						inet_ntop(AF_INET, &((struct  sockaddr_in*)addr_r)->sin_addr, sipr, sizeof(sipr));
						_plog->add(CLOG_DEFAULT_INF, "fd(%d) accept from %s:%u at listen fd(%d)", tf.kfd, sipr, uport, pol->kfd);
					}
					else if (addr_r->sa_family == AF_INET6) {
						uport = ntohs(((struct  sockaddr_in6*)addr_r)->sin6_port);
						inet_ntop(AF_INET6, &((struct  sockaddr_in6*)addr_r)->sin6_addr, sipr, sizeof(sipr));
						_plog->add(CLOG_DEFAULT_INF, "fd(%d) accept from [%s]:%u at listen fd(%d)", tf.kfd, sipr, uport, pol->kfd);
					}
					onAccept(tf.kfd, sipr, uport, pol->kfd);

					size_t sizeread = 0;
					psession pss = getSession(tf.kfd);
					if (pss && (sizeread = sizeCanRecv(pss)) > 0 && !pss->_readpause)
						postread_(tf.kfd, sizeread);
				} while (0);
				accept_(pol->kfd);//continue post a accept;
				return nret;
			}

			/**
			 * @brief 处理 false == GetQueuedCompletionStatus()
			 * @param pol
			 * @return 0:timeout ; 1:do one; -1:IOCP closed
			 * @remark if pol not null, free pol
			*/
			int oniocp_ststausfalse(t_overlap* pol)
			{
				DWORD dwerr = GetLastError();
				if (!pol) {
					if (WAIT_TIMEOUT == dwerr)
						return 0;
					else if (ERROR_ABANDONED_WAIT_0 == dwerr) { //完成端口关闭
						/*
						If a call to GetQueuedCompletionStatus fails because the completion port handle associated with it is closed while the call is outstanding,
						the function returns FALSE, *lpOverlapped will be NULL, and GetLastError will return ERROR_ABANDONED_WAIT_0.

						jy: 单线程中，先closehandle后，在调用GetQueuedCompletionStatus不会产生这个错误，反而会产生ERROR_INVALID_HANDLE错误。
						*/
						_plog->add(CLOG_DEFAULT_INF, "IOCP closeed, !GetQueuedCompletionStatus ERROR_ABANDONED_WAIT_0");
						return -1;
					}
					else if (ERROR_INVALID_HANDLE == dwerr) {
						_plog->add(CLOG_DEFAULT_ERR, "!GetQueuedCompletionStatus error ERROR_INVALID_HANDLE");
						return -1;
					}
					else if (WAIT_TIMEOUT != dwerr)
						_plog->add(CLOG_DEFAULT_ERR, "!GetQueuedCompletionStatus GetLastError %u", dwerr);
					return 0;
				}
				//下面处理失败的完成消息
				if (op_recv == pol->optype || op_send == pol->optype) { //连接断开会在这里回收投递
					t_fd* pfd;
					if (nullptr != (pfd = _mapfd.get(pol->kfd))) {//经实验不会执行这段代码，会在 oniocp_recv或oniocp_send中第一次断开通知
						_plog->add(CLOG_DEFAULT_DBG, "GetQueuedCompletionStatus FALSE, fd(%d) %s failed . GetLastError %u",
							pol->kfd, optype2str(pol->optype), dwerr);
						closefd(pfd->kfd, 102);//linux错误码network dropped
					}
				}
				else if (op_accept == pol->optype) { // accepte失败，关闭监听socket前会产生这个错误
					_plog->add(CLOG_DEFAULT_DBG, "GetQueuedCompletionStatus FALSE, TCP listen fd(%d) accept stop. GetLastError %u",
						pol->kfd, dwerr);
					if (INVALID_SOCKET != pol->sysfd) {
						closesocket(pol->sysfd);
						pol->sysfd = INVALID_SOCKET;
					}
				}
				else if (op_recvfrom == pol->optype) {
					_plog->add(CLOG_DEFAULT_DBG, "GetQueuedCompletionStatus FALSE, UDP fd(%d) readfrom failed. GetLastError:%u",
						pol->kfd, GetLastError());//如果数据超限会产生ERROR_MORE_DATA(234)错误
					t_fd* pfd = _mapfd.get(pol->kfd);
					if (nullptr != pfd) {
						if (pfd->post_rcv > 0)
							pfd->post_rcv -= 1;
						postreadfrom_(pfd->kfd);
					}
				}
				else {
					_plog->add(CLOG_DEFAULT_DBG, "GetQueuedCompletionStatus FALSE, fd(%d) %s failed . GetLastError %u",
						pol->kfd, optype2str(pol->optype), dwerr);
				}
				freeoverlap(pol);
				return 1;
			}

			int create_udp(const struct sockaddr* addr, socklen_t addrlen, t_fd& t, int ipv6only = 0) // create udp and bind if addr is not null , return fd
			{
				t.post_rcv = 0;
				t.post_snd = 0;
				t.kfd = nextfd();
				if (t.kfd < 0)
					return -1;
				t.fdtype = fd_udp;
				t.sa_family = addr ? addr->sa_family : AF_INET;
				t.sysfd = socket(t.sa_family, SOCK_DGRAM, IPPROTO_UDP);
				if (INVALID_SOCKET == t.sysfd)
					return -1;

				int opt = 1;
				if (addr && ipv6only && addr->sa_family == AF_INET6) {
					if (-1 == setsockopt(t.sysfd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&opt, sizeof(opt))) {
						closesocket(t.sysfd);
						return -1;
					}
				}
#ifdef _MEM_TINY
				int nbufsize = 160 * 1024;
#else
				int nbufsize = 512 * 1024;
#endif
				if (setsockopt(t.sysfd, SOL_SOCKET, SO_SNDBUF, (char*)&nbufsize, (socklen_t)sizeof(nbufsize)) < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "UDP fd(%d) set SO_SNDBUF error %d. ", t.kfd, WSAGetLastError());
				}
#ifdef _MEM_TINY
				nbufsize = 512 * 1024;
#else
				nbufsize = 1024 * 1024;
#endif
				if (setsockopt(t.sysfd, SOL_SOCKET, SO_RCVBUF, (char*)&nbufsize, (socklen_t)sizeof(nbufsize)) < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "UDP fd(%d) set SO_RCVBUF error %d. ", t.kfd, WSAGetLastError());
				}

				u_long iMode = 1;
				if (SOCKET_ERROR == ioctlsocket(t.sysfd, FIONBIO, &iMode)) {
					closesocket(t.sysfd);
					return -1;
				}
				if (addr && addrlen) {
					if (SOCKET_ERROR == setsockopt(t.sysfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt))
						|| SOCKET_ERROR == bind(t.sysfd, addr, addrlen)) {
						closesocket(t.sysfd);
						return -1;
					}
				}
				_mapfd.set(t.kfd, t);
				return t.kfd;
			}
		};
	}
}// namespace ec