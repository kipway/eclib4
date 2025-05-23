﻿/*
* @file ec_netepoll.h
* base net server class use epoll for linux
* 
* @author jiangyong
* @update
  2024-12-30 优化udp的发送
  2024-11-9 support no ec_alloctor
  2024-5-8 增加主动断开处理，用于发送websocket断开控制帧。
  2024-4-13 增加onSendBufSizeChanged()
  2024-4-3 更新closefd
  2023-12-21 增加总收发流量和总收发秒流量
  2023-6-15 add tcp keepalive
  2023-6-6  增加可持续fd, update closefd() 可选通知
  2023-5-21 update for download big http file
  2023-5.13 remove ec::memory
  2023-2-09 first version

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include "ec_aiolinux.h"
#include "ec_aiosession.h"
#include "ec_log.h"
#include "ec_map.h"
#include "ec_vector.hpp"
#ifndef SIZE_MAX_FD
#define SIZE_MAX_FD  16384 //最大fd连接数
#endif

#ifndef FRMS_UDP_READ_ONCE
#define FRMS_UDP_READ_ONCE  64 //UDP每次读的包数
#endif
#ifndef FRMS_UDP_SEND_ONCE
#define FRMS_UDP_SEND_ONCE 8
#endif
namespace ec {
	namespace aio {
		using NETIO = netio_linux;
		class serverepoll_
		{
		protected:
			ec::ilog* _plog;

			int _fdepoll;
			NETIO _net;

		private:
			int _lastwaiterr;
			struct epoll_event _fdevts[EC_AIO_EVTS];
		protected:
			char _recvtmp[EC_AIO_READONCE_SIZE];

		protected:
			/**
			 * @brief 主动断开，用于特殊处理websocket断开发送握手信号
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
				return EC_AIO_READONCE_SIZE;
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
			inline int setsendbuf(int fd, int n)
			{
				return _net.setsendbuf(fd, n);
			}

			inline int setrecvbuf(int fd, int n)
			{
				return _net.setrecvbuf(fd, n);
			}

			inline int connect_asyn(const struct sockaddr* addr, socklen_t addrlen) {
				return _net.connect_asyn(addr, addrlen);
			}

			/**
			 * @brief shutdown and close a kfd
			 * @param kfd  keyfd
			 * @return
			*/
			int close_(int kfd) //shutdown and close kfd
			{
				int ftype = _net.getfdtype(kfd);
				if (ftype < 0)
					return -1;
				_plog->add(CLOG_DEFAULT_DBG, "close_ fd(%d), fdtype = %d", kfd, ftype);
				_net.close_(kfd);
				return 0;
			}

			int epoll_add_tcpout(int kfd)
			{
				struct epoll_event evt;
				memset(&evt, 0, sizeof(evt));
				evt.events = EPOLLIN | EPOLLOUT | EPOLLERR;
				evt.data.fd = kfd;
				int nerr = 0;
				if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_ADD, kfd, &evt))) {
					_plog->add(CLOG_DEFAULT_ERR, "EPOLL_CTL_ADD failed. error = %d", nerr);
					_net.close_(kfd);
					return -1;
				}
				return 0;
			}

			inline bool setkeepalive(int fd, bool bfast = false)
			{
				return _net.setkeepalive(fd, bfast) >= 0;
			}
		public:
			serverepoll_(ec::ilog* plog) : _plog(plog), _fdepoll(-1), _lastwaiterr(-100)
			{
			}
			virtual ~serverepoll_() {

			}
			inline void SetFdFile(const char* sfile) {
				_net.SetFdFile(sfile);
			}
			//create epoll, return 0:ok; -1:error
			int open(const char* spre = nullptr)
			{
				if (_fdepoll >= 0)
					return 0;
				_fdepoll = _net.epoll_create_(1);
				if (_fdepoll < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "%sepoll_create_ failed.", spre? spre :"");
					return -1;
				}
				_plog->add(CLOG_DEFAULT_MSG, "%sepoll_create_ success.", spre ? spre : "");
				return 0;
			}

			/**
			 * @brief 关闭所有连接和epoll handel
			 * @return 0;
			 * @remark 用于退出时调用，不会通知应用层连接断开，应用层需自己释放和连接相关的资源。
			*/
			void close()
			{
				ec::vector<int> fds;
				fds.reserve(1024);
				_net.getall(fds);

				int fdtype = 0;
				for (auto& i : fds) {
					fdtype = _net.getfdtype(i);
					if (fdtype >= 0 && fdtype != _net.fd_epoll) { //fd除epoll外全部关闭
						_net.epoll_ctl_(_fdepoll, EPOLL_CTL_DEL, i, nullptr);
						_net.close_(i);
						_plog->add(CLOG_DEFAULT_DBG, "close fd(%d), fdtype = %d @serverepoll_::close", i, fdtype);
					}
				}
				if (_fdepoll >= 0)
					_net.close_(_fdepoll);
				_fdepoll = -1;
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
				int fdl = _net.bind_listen(paddr, addrlen, ipv6only);
				if (fdl < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "bind listen tcp://%s:%u failed.", netaddr.viewip(), port);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_MSG, "fd(%d) bind listen tcp://%s:%u success.", fdl, netaddr.viewip(), port);

				struct epoll_event evt;
				memset(&evt, 0, sizeof(evt));
				evt.events = EPOLLIN | EPOLLERR;
				evt.data.fd = fdl;

				int nerr = 0;
				if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_ADD, fdl, &evt))) {
					_plog->add(CLOG_DEFAULT_ERR, "epoll_ctrl_ failed. error = %d", nerr);
					_net.close_(fdl);
					return -1;
				}
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
				int fdl = _net.create_udp(paddr, addrlen, ipv6only);

				if (fdl < 0) {
					_plog->add(CLOG_DEFAULT_ERR, "bind udp://%s:%u failed.", netaddr.viewip(), port);
					return -1;
				}
				_plog->add(CLOG_DEFAULT_MSG, "fd(%d) bind udp://%s:%u success.", fdl, netaddr.viewip(), port);

				struct epoll_event evt;
				evt.events = EPOLLIN | EPOLLERR;
				evt.data.fd = fdl;

				int nerr = 0;
				if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_ADD, fdl, &evt))) {
					_plog->add(CLOG_DEFAULT_ERR, "epoll_ctrl_ failed. error = %d", nerr);
					_net.close_(fdl);
					return -1;
				}
				return fdl;
			}

			void runtime_(int waitmsec)
			{
				if (-1 == _fdepoll)
					return;

				int64_t curmstime = ec::mstime();
				if (llabs(curmstime - _lastmstime) >= 4) { //4毫秒处理一次接收流控
					dorecvflowctrl();
					_lastmstime = curmstime;
				}

				int nret = _net.epoll_wait_(_fdepoll, _fdevts, static_cast<int>(sizeof(_fdevts) / sizeof(struct epoll_event)), waitmsec);
				if (nret < 0) {
					if (_lastwaiterr != nret)
						_plog->add(CLOG_DEFAULT_ERR, "epoll_wait_ return %d", nret);
					_lastwaiterr = nret;
					return;
				}
				for (auto i = 0; i < nret; i++)
					onevent(_fdevts[i]);
			}

			/**
			 * @brief 设置可发送事件
			 * @param kfd keyfd
			*/
			void sendtrigger(int kfd)
			{
				psession pss = getSession(kfd);
				if (!pss)
					return;
				triger_evt(pss);
			}

			void udp_trigger(int kfd, bool bsend)
			{
				struct epoll_event evtmod;
				memset(&evtmod, 0, sizeof(evtmod));
				evtmod.events = bsend ? EPOLLIN | EPOLLOUT | EPOLLERR : EPOLLIN | EPOLLERR;
				evtmod.data.fd = kfd;
				int nerr = 0;
				if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_MOD, kfd, &evtmod)))
					_plog->add(CLOG_DEFAULT_ERR, "udp epoll_ctrl_ EPOLL_CTL_MOD failed @onevent. fd = %d,  error = %d", kfd, nerr);
			}

			void udp_trigger(int kfd)
			{
				psession pss = getSession(kfd);
				int nbuf = 0;
				if (pss) {
					udb_buffer_* pfrms = pss->getudpsndbuffer();
					if (pfrms && !pfrms->empty()) {
						nbuf = 1;
					}
				}
				struct epoll_event evtmod;
				memset(&evtmod, 0, sizeof(evtmod));
				evtmod.events = nbuf ? EPOLLIN | EPOLLOUT | EPOLLERR : EPOLLIN | EPOLLERR;
				evtmod.data.fd = kfd;
				int nerr = 0;
				if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_MOD, kfd, &evtmod)))
					_plog->add(CLOG_DEFAULT_ERR, "udp epoll_ctrl_ EPOLL_CTL_MOD failed @onevent. fd = %d,  error = %d", kfd, nerr);
			}

			/**
			 * @brief 发送，直到系统缓冲满或者发送应用缓冲发送完成。
			 * @param kfd keyfd
			 * @param overlap 未使用，兼容Windows版IOCP
			 * @return  >=0: post bytes;  -1:failed, close fd and call onDisconnected(kfd)
			*/
			int postsend(int kfd, int overlap = 0)
			{
				int ns = 0;
				psession pss = getSession(kfd);
				if (!pss)
					return -1;
				if ((ns = sendbuf(pss)) < 0) {
					closefd(kfd, 102);//network dropped
					return -1;
				}
				triger_evt(pss);
				return ns;
			}

			/**
			 * @brief 关闭连接，会产生onDisconnect和onDisconnected调用
			 * @param kfd  keyfd
			 * @param errorcode 网络错误码，0表示不是网络错误, 是服务端主动断开.
			 * @return 0:关闭; -1:不存在，之前已经被关闭.
			*/
			int closefd(int kfd, int errorcode)
			{
				if (!_net.hasfd(kfd))
					return -1;
				if (!errorcode) {
					onCloseFd(kfd);
				}
				onDisconnect(kfd);
				// Since Linux 2.6.9, event can be specified as NULL when using EPOLL_CTL_DEL
				_net.epoll_ctl_(_fdepoll, EPOLL_CTL_DEL, kfd, nullptr);
				_net.close_(kfd);
				onDisconnected(kfd);
				return 0;
			}

			size_t size_fds()
			{
				return _net.size();
			}

			inline int getbufsize(int fd, int op)
			{
				return _net.getbufsize(fd, op);
			}
		private:
			int64_t _lastmstime = 0;//上次扫描可发送的时间，单位GMT毫秒
			void dorecvflowctrl()//接收流控
			{
				for (auto& i : _net.getmap()) {
					if (i.fdtype != _net.fd_listen && i.fdtype != _net.fd_epoll && i.fdtype != _net.fd_udp) {
						triger_evt(getSession(i.kfd));
					}
				}
			}
		protected:
			void triger_evt(psession pss)
			{
				if (!pss)
					return;
				struct epoll_event evtmod;
				memset(&evtmod, 0, sizeof(evtmod));
				evtmod.events = EPOLLERR;

				if (sizeCanRecv(pss) > 0 && !pss->_readpause)
					evtmod.events |= EPOLLIN;
				if (!pss->_sndbuf.empty() || pss->_status == EC_AIO_FD_CONNECTING || pss->hasSendJob())
					evtmod.events |= EPOLLOUT;
				evtmod.data.fd = pss->_fd;

				int nerr = 0;
				if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_MOD, pss->_fd, &evtmod)))
					_plog->add(CLOG_DEFAULT_ERR, "epoll_ctrl_ EPOLL_CTL_MOD failed @onevent. fd = %d,  error = %d", pss->_fd, nerr);
			}
		private:
			void udp_sendto(int kfd)
			{
				psession pss = getSession(kfd);
				if (!pss) {
					return;
				}
				udb_buffer_* pfrms = pss->getudpsndbuffer();
				if (!pfrms || pfrms->empty()) {
					return;
				}
				int numsnd = 0, nbytes = 0;
				do {
					auto& frm = pfrms->front();
					if (!frm.empty()) {
						if (_net.sendto_(kfd, frm.data(), frm.size(), frm.getnetaddr(), frm.netaddrlen()) < 0) {
							if (EAGAIN != errno && EWOULDBLOCK != errno && ENOBUFS != errno) {
								onSendtoFailed(kfd, frm.getnetaddr(), frm.netaddrlen(), frm.data(), frm.size(), errno);
								pfrms->pop();
							}
							if (numsnd) {
								pss->onUdpSendCount(numsnd, nbytes);
								onSendCompleted(kfd, nbytes);
							}
							return;
						}
#ifdef _DEBUG
						if (_plog->getlevel() >= CLOG_DEFAULT_ALL) {
							ec::net::socketaddr peeraddr;
							peeraddr.set(frm.getnetaddr(), frm.netaddrlen());
							_plog->add(CLOG_DEFAULT_ALL, "fd(%d) sento %s:%u %zu bytes.", kfd,
								peeraddr.viewip(), peeraddr.port(), frm.size());
						}
#endif
						nbytes += (int)frm.size();
					}
					pfrms->pop();
					numsnd++;
				} while (!pfrms->empty() && numsnd < FRMS_UDP_SEND_ONCE && nbytes < 1024 * 32);
				pss->onUdpSendCount(numsnd, nbytes);
				onSendCompleted(kfd, nbytes);
			}

			char udpbuf_[1024 * 64] = { 0 };
			void onudpevent(struct epoll_event& evt)
			{
				if (evt.events & EPOLLIN) {
					int nr = -1, ndo = FRMS_UDP_READ_ONCE;
					socklen_t* paddrlen = nullptr;
					ec::net::socketaddr addr;
					struct sockaddr* paddr = addr.getbuffer(&paddrlen);
					do {
						nr = _net.recvfrom_(evt.data.fd, udpbuf_, (int)sizeof(udpbuf_), 0, paddr, paddrlen);
						if (nr > 0) {
#ifdef _DEBUG
							if (_plog->getlevel() >= CLOG_DEFAULT_ALL) {
								_plog->add(CLOG_DEFAULT_ALL, "fd(%d) recvfrom %s:%u %d bytes.", evt.data.fd,
									addr.viewip(), addr.port(), nr);
							}
#endif
							if (onReceivedFrom(evt.data.fd, udpbuf_, nr, paddr, *paddrlen) < 0)
								break;
						}
						else if (nr < 0 && EAGAIN != errno && EWOULDBLOCK != errno) {
							_plog->add(CLOG_DEFAULT_ERR, "fd(%d) recvfrom failed. error %d", evt.data.fd, errno);
						}
						--ndo;
					} while (nr > 0 && ndo > 0);
				}
				if (evt.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
					_plog->add(CLOG_DEFAULT_ERR, "udp fd(%d)  error events %08XH", evt.data.fd, evt.events);
				}
				if (evt.events & EPOLLOUT) {
					udp_sendto(evt.data.fd);
				}
			}

			void onevent(struct epoll_event& evt)
			{
				int nfdtype = _net.getfdtype(evt.data.fd);
				if (nfdtype == _net.fd_udp) {
					onudpevent(evt);
					udp_trigger(evt.data.fd);
					return;
				}
				if ((evt.events & EPOLLIN) && _net.fd_listen == nfdtype) {
#ifdef _DEBUG
					_plog->add(CLOG_DEFAULT_ALL, "listen fd(%d)  EPOLLIN, events %08XH", evt.data.fd, evt.events);
#endif
					ec::net::socketaddr clientaddr;
					socklen_t* paddrlen = nullptr;
					struct sockaddr* paddr = clientaddr.getbuffer(&paddrlen);
					int fdc = _net.accept_(evt.data.fd, paddr, paddrlen);
					if (fdc >= 0) {
						int nerr = 0;
						struct epoll_event ev;
						memset(&ev, 0, sizeof(ev));
						ev.events = EPOLLIN | EPOLLOUT | EPOLLERR;
						ev.data.fd = fdc;
						if (0 != (nerr = _net.epoll_ctl_(_fdepoll, EPOLL_CTL_ADD, fdc, &ev))) {
							_plog->add(CLOG_DEFAULT_ERR, "epoll_ctrl_ EPOLL_CTL_ADD failed @onconnect_in. fd = %d, error = %d", fdc, nerr);
							_net.close_(fdc);
						}
						else {
							uint16_t uport = 0;
							char sip[48] = { 0 };
							clientaddr.get(uport, sip, sizeof(sip));
							_plog->add(CLOG_DEFAULT_INF, "fd(%d) accept from %s:%u at listen fd(%d)",
								fdc, clientaddr.viewip(), uport, evt.data.fd);
							onAccept(fdc, sip, uport, evt.data.fd);
						}
					}
					else
						_plog->add(CLOG_DEFAULT_ERR, "accept failed. listen fd = %d", evt.data.fd);
					return;
				}
				if (evt.events & EPOLLIN) {
					int nr = -1;
					psession pss = getSession(evt.data.fd);
					if (pss && !pss->_readpause) {
						size_t zr = sizeCanRecv(pss);
						if (zr > 0) {
							if (zr > sizeof(_recvtmp))
								zr = sizeof(_recvtmp);
							nr = _net.recv_(evt.data.fd, _recvtmp, zr, 0);
							if (!nr || (nr < 0 && EAGAIN != _net.geterrno() && EWOULDBLOCK != _net.geterrno())) {
								if (nr) {
									_plog->add(CLOG_DEFAULT_WRN, "fd(%d) disconnected at EPOLLIN recv return %d, errno %d", evt.data.fd,
										nr, _net.geterrno());
								}
								else {
									_plog->add(CLOG_DEFAULT_DBG, "fd(%d) disconnected gracefully at EPOLLIN recv return 0", evt.data.fd);
								}
								closefd(evt.data.fd, 102);//network sropped
								return;
							}
#ifdef _DEBUG
							_plog->add(CLOG_DEFAULT_ALL, "fd(%d) received %d bytes", evt.data.fd, nr);
#endif
							if (nr > 0 && onReceived(evt.data.fd, _recvtmp, nr) < 0) {
								closefd(evt.data.fd, 0); //主动断开
								return;
							}
						}
						else {
							_plog->add(CLOG_DEFAULT_ALL, "fd(%d) %s pause reading for task balancing.",
								pss->_fd, pss->ProtocolName(pss->_protocol));
						}
					}
				}
				if (evt.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
					_plog->add(CLOG_DEFAULT_DBG, "fd(%d)  error events %08XH %s %s %s %s %s", evt.data.fd, evt.events,
						(evt.events & EPOLLERR)? "EPOLLERR" :"", (evt.events & EPOLLHUP) ? "EPOLLHUP" : "",
						(evt.events & EPOLLRDHUP) ? "EPOLLRDHUP" : "", (evt.events & EPOLLIN) ? "EPOLLIN" : "",
						(evt.events & EPOLLOUT) ? "EPOLLOUT" : "");
					closefd(evt.data.fd, 102);//network dropped
					return;
				}
				if (evt.events & EPOLLOUT) {
#ifdef _DEBUG
					_plog->add(CLOG_DEFAULT_ALL, "fd(%d)  EPOLLOUT, events %08XH", evt.data.fd, evt.events);
#endif
					if (onepollout(evt.data.fd) < 0)
						return;
				}
				if (NETIO::fd_listen != nfdtype && NETIO::fd_epoll != nfdtype)
					sendtrigger(evt.data.fd);
			}

			int onepollout(int kfd)
			{
				psession pss = getSession(kfd);
				if (!pss)
					return -1;
				int nfdtype = _net.getfdtype(kfd);
				if (_net.fd_tcpout == nfdtype) { // asyn connect out
					if (pss->_status == EC_AIO_FD_CONNECTING) {
						int serr = 0;
						socklen_t serrlen = sizeof(serr);
						getsockopt(_net.getsysfd(kfd), SOL_SOCKET, SO_ERROR, (void*)&serr, &serrlen);
						if (serr) {
							closefd(kfd, 111);//connection refused
							return -1;
						}
						pss->_status = EC_AIO_FD_CONNECTED;
						onTcpOutConnected(kfd);
						return 0;
					}
				}
				if (sendbuf(pss) < 0) {
					closefd(kfd, 102);//network dropped
					return -1;
				}
				if (pss->_sndbuf.empty()) {
					if (!pss->onSendCompleted()) {
						if (_plog) {
							_plog->add(CLOG_DEFAULT_WRN, "fd(%d) onSendCompleted false.", kfd);
						}
						closefd(kfd, 0);//主动断开
						return -1;
					}
				}
				return 0;
			}

			/**
			 * @brief 发送,直到系统缓冲满或者发送应用缓冲发送完成。
			 * @param pss
			 * @param pdata
			 * @param size
			 * @return 返回发送的总字节数, -1:error
			*/
			int sendbuf(psession pss, const void* pdata = nullptr, size_t size = 0)
			{
				int ns = 0, fd = pss->_fd, nsnd = 0;
				if (pdata && size)
					pss->_sndbuf.append((const uint8_t*)pdata, size);

				const void* pd = nullptr;
				size_t zlen = 0;

				pd = pss->_sndbuf.get(zlen);
				while (pd && zlen) {
					ns = _net.send_(fd, pd, (int)(zlen), MSG_DONTWAIT | MSG_NOSIGNAL);
#ifdef _DEBUG
					_plog->add(CLOG_DEFAULT_ALL, "sendbuf fd(%d) size %d", fd, ns);
#endif
					if (ns < 0) {
						int nerr = _net.geterrno();
						if (nerr != EAGAIN)
							_plog->add(CLOG_DEFAULT_ERR, "fd(%d) sendbuf syserr %d", fd, nerr);
						else
							ns = 0;
						break;
					}
					else if (!ns)
						break;
					nsnd += ns;
					pss->_sndbuf.freesize(ns);
					if (ns < (int)(zlen))
						break;
					pd = pss->_sndbuf.get(zlen);
				}
				if (nsnd) {
					pss->_allsend += nsnd;
					pss->_bpsSnd.add(ec::mstime(), nsnd);
					if (pss->_lastsndbufsize != pss->_sndbuf.size()) {
						pss->_lastsndbufsize = pss->_sndbuf.size();
						onSendBufSizeChanged(fd, pss->_lastsndbufsize, pss->_protocol);
					}
					onSendCompleted(fd, nsnd);
				}
				return ns < 0 ? -1 : nsnd;
			}
		private:

		};
	}//namespace sio
}//namespace ec
