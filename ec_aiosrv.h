﻿/*
* @file ec_aiosrv.h
* @brief a new net server ,IOCP in windows, EPOLL in linux
* 
* @author jiangyong
* 
* class ec::aio::netserver

* @update
	2024-5-15 增加高优先级会话处理。
	2024-5-8 增加主动断开处理，用于发送websocket断开控制帧。
	2024-4-29 添加 setSessionDelayDisconnect()
	2024-4-9 消息处理后的发送触发移到底层平台相关层。
	2024-4-3 修正2023-12-13增加会话均衡doRecvBuffer产生的BUG
	2023-12-21 增加总收发流量和总收发秒流量
	2023-12-13 增加连接会话消息处理均衡,每个连接每次解析和处理一个消息。
	2023-6-16 add tcp keepalive
	2023-6-7 fix netserver::tcpconnect(uint16_t port, const char* sip) connect localhost failed in windows while sip==nul
	2023-6-6  增加可持续fd
    2023-5-21 update for download big http file
    2023-2-08 first version

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once

#include "ec_aiosession.h"

#ifdef _WIN32
#include "ec_netiocp.h"
#else
#include "ec_netepoll.h"
#endif

#if (0 != EC_AIOSRV_TLS)
#include "ec_aiotls.h"
#endif

#if (0 != EC_AIOSRV_HTTP)
#include "ec_aiohttp.h"
#if (0 != EC_AIOSRV_TLS)
#include "ec_aiohttps.h"
#endif
#endif

namespace ec {
	namespace aio {
#ifdef _WIN32
		using netserver_ = serveriocp_;
#else
		using netserver_ = serverepoll_;
#endif
		class netserver : public netserver_
		{
		protected:
			ec::blk_alloctor<> _sndbufblks; //共享发送缓冲分配区
			ec::hashmap<int, psession, kep_session, del_session > _mapsession;//会话连接
#if (0 != EC_AIOSRV_TLS)
			ec::tls_srvca _ca;  // certificate
#endif
			int64_t _mstimelastdelete = 0;//上次扫描删除连接的时间
			uint64_t _allsend = 0 ;//总发送
			uint64_t _allrecv = 0;//总接收
			t_bps   _bpsRcv; //总接受秒流量
			t_bps   _bpsSnd; //总发送秒流量
		public:
			netserver(ec::ilog* plog) : netserver_(plog)
				, _sndbufblks(EC_AIO_SNDBUF_BLOCKSIZE - EC_ALLOCTOR_ALIGN, EC_AIO_SNDBUF_HEAPSIZE / EC_AIO_SNDBUF_BLOCKSIZE)
			{
			}
			virtual ~netserver() {
				_mapsession.clear();
			}
			inline void setLog(ec::ilog* plog)
			{
				_plog = plog;
			}
			inline ec::ilog* getLog()
			{
				return _plog;
			}
#if (0 != EC_AIOSRV_TLS)
			bool initca(const char* filecert, const char* filerootcert, const char* fileprivatekey)
			{
				if (!_ca.InitCert(filecert, filerootcert, fileprivatekey)) {
					if (_plog)
						_plog->add(CLOG_DEFAULT_ERR, "Load certificate failed (%s,%s,%s)", filecert,
							filerootcert != nullptr ? filerootcert : "none", fileprivatekey);
					return false;
				}
				return true;
			}
#endif
			void runtime(int waitmsec, int64_t& currentmsec)
			{
				if (!currentmsec)
					currentmsec = ec::mstime();
				timerjob(currentmsec);
				int nmsg = doRecvBuffer(); //处理会话接收缓冲中未处理完的消息。
				netserver_::runtime_(nmsg > 0 ? 0 : waitmsec);
				if (llabs(currentmsec - _mstimelastdelete) >= 1000) { //每秒扫描一次错误会话
					_mstimelastdelete = currentmsec;
					time_t curt = ::time(nullptr);
					ec::vector<int> dels;
					dels.reserve(32);
					for (const auto& i : _mapsession) {
						if (i->_time_error && llabs(curt - i->_time_error) >= 5) {
							dels.push_back(i->_fd);
						}
					}
					for (const auto& fd : dels) {
						_plog->add(CLOG_DEFAULT_INF, "close fd(%d) delayed disconnect.", fd);
						closefd(fd, 0); //主动断开
					}
				}
			}

			psession getsession(int fd)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return nullptr;
				return pss;
			}

			int getsessionstatus(int fd)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return -1;
				return pss->_status;
			}

			int setsessionstatus(int fd, int st)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return -1;
				pss->_status = st;
				return 0;
			}

			void setSessionDelayDisconnect(int fd, int delaysec = 5) {
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return;
				pss->_time_error = ::time(nullptr) + delaysec - 5;
			}

			int getsessionprotocol(int fd)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return -1;
				return pss->_protocol;
			}

			int setsessionprotocol(int fd, int protocol)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return -1;
				pss->_protocol = protocol;
				return 0;
			}

			void setreadpause(int* protocols, int numprotocols, int readpause)
			{
				int j;
				for (auto& i : _mapsession) {
					for (j = 0; j < numprotocols; j++) {
						if (protocols[j] == i->_protocol) {
							i->_readpause = readpause;
							break;
						}
					}
				}
			}

			int waterlevel(int fd)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return -1;
				return pss->_sndbuf.waterlevel();
			}

			template<class _ClsPtr>
			bool getextdata(int fd, const char* clsname, _ClsPtr& ptr)
			{
				psession pi = nullptr;
				ptr = nullptr;
				if (!_mapsession.get(fd, pi))
					return false;
				return pi->getextdata(clsname, ptr);
			}

			bool setextdata(int fd, ssext_data* pdata)
			{
				psession pi = nullptr;
				if (!_mapsession.get(fd, pi))
					return false;
				pi->setextdata(pdata);
				return true;
			}

			/**
			 * @brief 经session编码打包后提交到发送缓冲,如果可能会立即发送。
			 * @param fd 
			 * @param pdata 
			 * @param size 
			 * @return >=0 实际发送的字节数(0 可能小于size，剩下的提交到发送缓冲) ; -1: 发送错误，断开并回收连接，并调用 onDisconnected
			*/
			int sendtofd(int fd, const void* pdata, size_t size)
			{
				psession pss = nullptr;
				if (!_mapsession.get(fd, pss))
					return -1;
				if(pss->sendasyn(pdata, size, _plog) < 0)
					return -1;
				return postsend(fd);
			}

			/**
			 * @brief 异步连接, 会建立一个默认的tcp session, 连接成功后会使用onTcpConnectOut通知
			 * @return 返回keyfd
			*/
			int tcpconnect(uint16_t port, const char* sip)
			{
				ec::net::socketaddr netaddr;
				if (!sip || !*sip)
					sip = "127.0.0.1";
				if (netaddr.set(port, sip) < 0)
					return -1;
				int addrlen = 0;
				struct sockaddr* paddr = netaddr.getsockaddr(&addrlen);
				if (!paddr)
					return -1;
				
				int fd = connect_asyn(paddr, addrlen);
				if (fd < 0){
					_plog->add(CLOG_DEFAULT_ERR, "connect tcp://%s:%u failed.", netaddr.viewip(), port);
					return -1;
				}
				psession pss = new session(&_sndbufblks, fd);
				if (!pss) {
					_plog->add(CLOG_DEFAULT_ERR, "new session memory error");
					close_(fd);
					return -1;
				}
				setkeepalive(fd);
#ifndef _WIN32
				if (epoll_add_tcpout(fd) < 0) {
					delete pss;
					return -1;
				}
#endif
				_mapsession.set(fd, pss);
				return fd;
			}

		protected:
			/**
			 * @brief 处理会话接收缓冲中可能分离出的消息，返回处理的消息数
			 * @return 返回处理的消息数; 
			*/
			int doRecvBuffer()
			{
				int msgtype, n = 0, nfd, nup;
				ec::bytes msg;
				ec::vector<int> dels;
				dels.reserve(32);

				ec::aio::psession pss = nullptr;
				uint64_t pos = _mapsession._begin(), posp = pos;
				while (_mapsession.next(pos, pss)) {
					if (pss->_time_error) {
						posp = pos;
						continue;
					}
					nup = pss->msglevel();
					nfd = pss->_fd;
					do {
						msgtype = pss->onrecvbytes(nullptr, 0, _plog, &msg);
						if (EC_AIO_MSG_NUL == msgtype) {
							break;
						}
						else if(EC_AIO_MSG_CLOSE == msgtype) {
							dels.push_back(nfd);
							_plog->add(CLOG_DEFAULT_DBG, "fd(%d) read websocket close message in doRecvBuffer.", nfd);
							break;
						}
						else if (EC_AIO_MSG_ERR == msgtype) {
							dels.push_back(nfd);
							_plog->add(CLOG_DEFAULT_DBG, "fd(%d) parse message failed in doRecvBuffer.", nfd);
							break;
						}
						else {
							++n;
							_plog->add(CLOG_DEFAULT_ALL, "fd(%d) %s parse one recvbuf msgtype = %d success",
								pss->_fd, pss->ProtocolName(pss->_protocol), msgtype);
							if (domessage(nfd, msg, msgtype) < 0) {
								if (!_mapsession.has(nfd)) { //delete the nfd in domessage or postsend
									pos = posp;
									_plog->add(CLOG_DEFAULT_ALL, "fd(%d) disconnected at doRecvBuffer", nfd);
								}
								else
									dels.push_back(nfd);
								break;
							}
							msg.clear();
							--nup;
						}
					} while (nup > 0);
					posp = pos;
				}
				for (const auto& fd : dels) {
					if (0 == closefd(fd, 0)) //主动断开
						_plog->add(CLOG_DEFAULT_INF, "close fd(%d) at doRecvBuffer.", fd);
				}
				return n;
			}

			/**
			 * @brief size can receive ,use for flowctrl
			 * @param pss
			 * @return >0 size can receive;  0: pause read
			*/
			virtual size_t  sizeCanRecv(psession pss) {
				
				if (!pss->_lastappmsg || pss->_rbuf.empty())
					return EC_AIO_READONCE_SIZE;
				return 0;
			};

			/**
			 * @brief 是否容许协议接入
			 * @param fdlisten 监听端口
			 * @param nproco 具体协议如 EC_AIO_PROC_HTTP 等
			 * @return 返回true可正常升级协议，否则不响应，延迟断开
			*/
			virtual bool EnableProtocol(int fdlisten, int nproco) {
				return true;
			}

			virtual void onprotocol(int fd, int nproco) {};

			/**
			 * @brief 处理消息,已经分包完成
			 * @param fd 虚拟fd
			 * @param sbuf 消息包
			 * @param msgtype 消息类型 EC_AIO_MSG_XXX defined in ec_aiosession.h
			 * @return 0:ok; -1:error, will disconnect
			*/
			virtual int domessage(int fd, ec::bytes& sbuf, int msgtype) = 0;

#if (0 != EC_AIOSRV_TLS)
			virtual ec::tls_srvca* getCA(int fdlisten) { //支持多证书
				return &_ca;
			}
#endif
			virtual void timerjob(int64_t currentms)
			{
			}

			void onCloseFd(int kfd) override
			{
				if (getsessionstatus(kfd) >= EC_AIO_FD_CONNECTED) {
					psession pss = nullptr;
					if (_mapsession.get(kfd, pss)) {
						if (pss && (EC_AIO_PROC_WS == pss->_protocol || EC_AIO_PROC_WSS == pss->_protocol)) {
							if (pss->onClose(1000, nullptr, 0)) {//发送正常断开握手信息
								postsend(kfd, 10);
							}
						}
					}
				}
			}

			/**
			 * @brief 连接即将关闭(session和fd均还未删除)，应用层有关联此kfd的做清理操作。
			 * @param kfd keyfd
			 * @remark 调用时fd还没有删除断开, 还可以发送信息。
			*/
			virtual void onDisconnect(int kfd) {
			}

			/**
			 * @brief 已经删除网络IO的fd连接，但还没删除协议层的session，应用层重载时需要调用ec::aio::netserver的本函数
			 * @param fd 会话id(不是系统fd)
			*/
			virtual void onDisconnected(int fd)
			{
				_plog->add(CLOG_DEFAULT_DBG, "netserver::onDisconnected fd(%d)", fd);
				_mapsession.erase(fd);//从协议层清空
			}
			
			/**
			 * @brief 协议升级，应用层重载是先做应用层的协议升级判断，再调用父类做TLS和HTTP升级。
			 * @param fd 
			 * @param pi 
			 * @return 1：协议升级成功; 0:待判断; -1:错误
			 */
			virtual int onupdate_proctcp(int fd, psession* pi) //base TCP protocol. return -1 :error ; 0:no; 1:ok
			{
#if (0 != EC_AIOSRV_TLS) || (0 != EC_AIOSRV_HTTP)
				const uint8_t* pu = (const uint8_t*)(*pi)->_rbuf.data_();
#endif
				size_t size = (*pi)->_rbuf.size_();
				if (size < 5u)
					return 0;
#if (0 != EC_AIOSRV_TLS)
				if (pu[0] == 22 && pu[1] == 3 && pu[2] > 0 && pu[2] <= 3) { // update client TLS protocol
					if (!EnableProtocol((*pi)->_fdlisten, EC_AIO_PROC_TLS)) {
						(*pi)->_time_error = ::time(nullptr);//设置延迟断开开始时间
						return 0; //不应答,延迟断开
					}
					ec::tls_srvca* pCA = getCA((*pi)->_fdlisten);
					if (!pCA) {
						if (_plog)
							_plog->add(CLOG_DEFAULT_MOR, "fd(%d) update TLS1.2 protocol getCA failed, no server certificate.", fd);
						(*pi)->_time_error = ::time(nullptr);//设置延迟断开开始时间
						return 0; //不应答,延迟断开
					}
					if (pCA->empty()) {
						if (_plog)
							_plog->add(CLOG_DEFAULT_MOR, "fd(%d) update TLS1.2 protocol failed, no server certificate", fd);
						(*pi)->_time_error = ::time(nullptr);//设置延迟断开开始时间
						return 0; //不应答,延迟断开
					}

					(*pi)->_time_error = 0;
					psession ptls = new session_tls(fd, std::move(**pi), pCA, _plog);
					if (!ptls)
						return -1;
					_mapsession.set(ptls->_fd, ptls);
					*pi = ptls;
					if (_plog)
						_plog->add(CLOG_DEFAULT_MSG, "fd(%d) update TLS1.2 protocol success", fd);
					onprotocol(fd, ptls->_protocol);
					return 1;
				}
#endif
#if (0 != EC_AIOSRV_HTTP)
				if ((ec::strineq("head ", (const char*)pu, size, 5)
					|| ec::strineq("get ", (const char*)pu, size, 4))
					) { //update http
					ec::http::package r;
					if (r.parse((const char*)pu, size) < 0 || !EnableProtocol((*pi)->_fdlisten, EC_AIO_PROC_HTTP)) {
						(*pi)->_time_error = ::time(nullptr);//设置延迟断开开始时间
						return 0; //不应答,延迟断开
					}
					(*pi)->_time_error = 0;
					psession phttp = new session_http(std::move(**pi));
					if (!phttp)
						return -1;
					_mapsession.set(phttp->_fd, phttp);
					*pi = phttp;
					if (_plog)
						_plog->add(CLOG_DEFAULT_MSG, "fd(%u) update HTTP protocol success", fd);
					onprotocol(fd, phttp->_protocol);
					return 1;
				}
#endif
				(*pi)->_time_error = ::time(nullptr);//不支持的协议延迟断开.
				return 0;
			}
#if (0 != EC_AIOSRV_TLS)
			/**
			 * @brief 协议升级，应用层重载是先做应用层的协议升级判断，再调用父类做TLS和HTTP升级。
			 * @param fd
			 * @param pi
			 * @return 1：协议升级成功; 0:待判断; -1:错误
			 */
			virtual int onupdate_proctls(int fd, psession* pi) //base TLS protocol. return -1 :error ; 0:no; 1:ok
			{
				size_t size = (*pi)->_rbuf.size_();
				if (size < 5u)
					return 0;
#if (0 != EC_AIOSRV_HTTP)
				const uint8_t* pu = (const uint8_t*)(*pi)->_rbuf.data_();
				if ((ec::strineq("head ", (const char*)pu, size, 5)
					|| ec::strineq("get ", (const char*)pu, size, 4))
					) { //update http
					ec::http::package r;
					if (r.parse((const char*)pu, size) < 0 || !EnableProtocol((*pi)->_fdlisten, EC_AIO_PROC_HTTPS)) {
						(*pi)->_time_error = ::time(nullptr);//设置延迟断开开始时间
						return 0; //不应答,延迟断开
					}
					(*pi)->_time_error = 0;
					psession phttp = new session_https(std::move(*((session_tls*)*pi)));
					if (!phttp)
						return -1;
					_mapsession.set(phttp->_fd, phttp);
					*pi = phttp;
					if (_plog)
						_plog->add(CLOG_DEFAULT_MSG, "fd(%u) update HTTPS protocol success", fd);
					onprotocol(fd, phttp->_protocol);
					return 1;
				}
#endif
				(*pi)->_time_error = ::time(nullptr);//不支持的协议延迟断开.
				return 0;
			}
#endif

			/**
			 * @brief 接收数据
			 * @param kfd   keyfd
			 * @param pdata Received data
			 * @param size  Received data size
			 * @return 0:OK; -1:error，will be close 
			*/
			virtual int onReceived(int kfd, const void* pdata, size_t size)
			{
				int64_t mscurtime = ec::mstime();
				_allrecv += size;
				_bpsRcv.add(mscurtime, (int64_t)size);

				psession pss = nullptr;
				if (!_mapsession.get(kfd, pss))
					return -1;
				if (pss->_time_error) {
					return EC_AIO_MSG_NUL;
				}
				pss->_allrecv += size;
				pss->_bpsRcv.add(mscurtime, (int64_t)size);
				ec::bytes msg;
				int msgtype = pss->onrecvbytes(pdata, size, _plog, &msg);
				if (EC_AIO_PROC_TCP == pss->_protocol && EC_AIO_MSG_TCP == msgtype) {
					pss->_rbuf.append(msg.data(), msg.size());
					int nup = onupdate_proctcp(pss->_fd, &pss);
					if (nup != 1)
						return nup;
					msg.clear();
					msgtype = pss->onrecvbytes(nullptr, 0, _plog, &msg);
				}
#if (0 != EC_AIOSRV_TLS)
				else if (EC_AIO_PROC_TLS == pss->_protocol && EC_AIO_MSG_TCP == msgtype) {
					pss->_rbuf.append(msg.data(), msg.size());
					int nup = onupdate_proctls(pss->_fd, &pss);
					if (nup != 1)
						return nup;
					msg.clear();
					msgtype = pss->onrecvbytes(nullptr, 0, _plog, &msg);
				}
#endif
				if (msgtype > EC_AIO_MSG_NUL) {
					int ndo = pss->msglevel(); //处理ndo个消息,剩下得在doRecvBuffer中处理。
					do {
						--ndo;
						if (domessage(pss->_fd, msg, msgtype) < 0) {
							_plog->add(CLOG_DEFAULT_WRN, "fd(%d) domessage message failed.", pss->_fd);
							return -1;
						}
						msg.clear();
						if(ndo > 0)
							msgtype = pss->onrecvbytes(nullptr, 0, _plog, &msg);
					} while (ndo > 0 && msgtype > EC_AIO_MSG_NUL);
				}
				if (msgtype == EC_AIO_MSG_ERR) {
					_plog->add(CLOG_DEFAULT_ERR, "fd(%d) read error message.", pss->_fd);
					return -1;
				}
				else if (EC_AIO_MSG_CLOSE == msgtype) {
					_plog->add(CLOG_DEFAULT_DBG, "fd(%d) read websocket close message.", pss->_fd);
					return -1;
				}
				return 0;
			}

			/**
			 * @brief get session
			 * @param kfd keyfd
			 * @return nullptr or psession with kfd
			*/
			virtual psession getSession(int kfd)
			{
				psession ps = nullptr;
				if (!_mapsession.get(kfd, ps)) {
					return nullptr;
				}
				return ps;
			}

			virtual void onAccept(int fd, const char* sip, uint16_t port, int fdlisten)
			{
				setkeepalive(fd);
				psession pss = new session(&_sndbufblks, fd, fdlisten);
				if (!pss)
					return;
				pss->_status = EC_AIO_FD_CONNECTED;
				ec::strlcpy(pss->_peerip, sip, sizeof(pss->_peerip));
				pss->_peerport = port;
				_mapsession.set(fd, pss);
			}

			virtual void onSendCompleted(int kfd, size_t size)
			{
				_allsend += size;
				_bpsSnd.add(ec::mstime(), (int64_t)size);
			}

			virtual int onReceivedFrom(int kfd, const void* pdata, size_t size, const struct sockaddr* addrfrom, int addrlen) {				
				_allrecv += size;
				_bpsRcv.add(ec::mstime(), (int64_t)size);
				return 0;
			}
		};
	}//namespace aio
}//namespace ec