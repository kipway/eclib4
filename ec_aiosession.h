﻿/*!
\file ec_aiosession.h

Asynchronous session base class

\author  jiangyong
\update
  2024.11.9 support no ec_alloctor
  2024-5-15 增加高优先级会话处理。
  2024-5-8 添加会话onClose接口,处理websocket协议断开握手控制帧。
  2023-12-13 增加会话连接消息处理均衡
  2023-5-21 update for http download big file

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once

#include "ec_string.h"
#include "ec_string.hpp"
#include "ec_log.h"
#include "ec_queue.h"

#ifndef EC_AIO_READONCE_SIZE 
#define EC_AIO_READONCE_SIZE (1024 * 14)
#endif

#ifndef EC_AIO_SNDBUF_BLOCKSIZE
#define EC_AIO_SNDBUF_BLOCKSIZE (1024 * 64) // 64K
#endif

#ifndef EC_AIO_SNDBUF_HEAPSIZE
#define EC_AIO_SNDBUF_HEAPSIZE (1024 * 1024 * 4) // 4M
#endif

#ifndef EC_AIO_SNDBUF_MAXSIZE
#define EC_AIO_SNDBUF_MAXSIZE (1024 * 1024 * 32) // 32M
#endif

#ifndef EC_AIO_EVTS
#define EC_AIO_EVTS 16
#endif

//fd status
#define EC_AIO_FD_DISCONNECT (-1) //断开,用于网络IO检查到对端错误时设置
#define EC_AIO_FD_CONNECTING 0
#define EC_AIO_FD_CONNECTED  1
#define EC_AIO_FD_TLSHANDOK  2

//messgae type
#define EC_AIO_MSG_CLOSE (-2)
#define EC_AIO_MSG_ERR   (-1)
#define EC_AIO_MSG_NUL   0
#define EC_AIO_MSG_TCP   1
#define EC_AIO_MSG_HTTP  2 // include HTTPS
#define EC_AIO_MSG_WS    3 // include WSS
#define EC_AIO_MSG_UDP   4

//protocol type
#define EC_AIO_PROC_TCP  0
#define EC_AIO_PROC_TLS  1
#define EC_AIO_PROC_UDP  2

#define EC_AIO_PROC_HTTP  16
#define EC_AIO_PROC_HTTPS 17

#define EC_AIO_PROC_WS  32
#define EC_AIO_PROC_WSS 33

#ifndef EC_UDP_FRM_INBUF_SIZE
#define EC_UDP_FRM_INBUF_SIZE 64
#endif

#ifndef NETIO_BPS_ITEMS
#define NETIO_BPS_ITEMS 10 //秒流量计算粒度，每秒数据数。
#endif
namespace ec {
	namespace aio {

		class udp_frm_
		{
		public:
			sockaddr_in6 _netaddr;
			int _netaddrlen;
			void* _pfrmbuf;
			size_t _frmsize;
			char  _inbuf[EC_UDP_FRM_INBUF_SIZE];
			udp_frm_() : _netaddrlen(0), _pfrmbuf(nullptr), _frmsize(0) {
				memset(&_netaddr, 0, sizeof(_netaddr));
			}
			udp_frm_(const struct sockaddr* paddr, int addrlen, const void* pfrm, size_t frmsize) {
				setnetaddr(paddr, addrlen);
				_frmsize = (uint32_t)frmsize;
				_inbuf[0] = 0;
				if (_frmsize <= sizeof(_inbuf)) {
					_pfrmbuf = nullptr;
					memcpy(_inbuf, pfrm, frmsize);
				}
				else {
					_pfrmbuf = ec::g_malloc(_frmsize);
					if (!_pfrmbuf) {
						_frmsize = 0;
						return;
					}
					memcpy(_pfrmbuf, pfrm, frmsize);
				}
			}
			virtual ~udp_frm_()
			{
				if (_pfrmbuf) {
					ec::g_free(_pfrmbuf);
					_pfrmbuf = nullptr;
				}
				_frmsize = 0;
			}

			void* data()
			{
				if (_frmsize <= sizeof(_inbuf))
					return (void*)_inbuf;
				return _pfrmbuf;
			}

			inline size_t size()
			{
				return _frmsize;
			}

			bool empty()
			{
				return !_frmsize;
			}
			const struct sockaddr* getnetaddr() {
				return (const struct sockaddr*)&_netaddr;
			}
			int netaddrlen() {
				return _netaddrlen;
			}
			void setnetaddr(const struct sockaddr* paddr, size_t addrsize)
			{
				if (addrsize > 0 && addrsize <= (int)sizeof(_netaddr)) {
					memcpy(&_netaddr, paddr, addrsize);
					_netaddrlen = (int)addrsize;
				}
			}
		};
		using udb_buffer_ = ec::queue<udp_frm_>;

		class ssext_data // application session extension data
		{
		public:
			_USE_EC_OBJ_ALLOCATOR
			ssext_data() {
			}
			virtual ~ssext_data() {
			}
			virtual const char* classname() {
				return "ssext_data";
			}
		};

		struct t_bps
		{
			struct t_i {
				int64_t _t;
				int64_t _v;
			};
			t_bps() {
				memset(&_tv, 0, sizeof(_tv));
			}
			t_i _tv[NETIO_BPS_ITEMS]; // 0->NETIO_BPS_ITEMS-1对应的按照时间间隔的数据，最近的在0
			void add(int64_t t, int64_t v) //添加最新的到最前面
			{
				if (llabs(t - _tv[0]._t) <= 1000 / NETIO_BPS_ITEMS) {
					_tv[0]._v += v;
				}
				else {
					memmove(&_tv[1], &_tv[0], sizeof(t_i) * (NETIO_BPS_ITEMS - 1));
					_tv[0]._t = t;
					_tv[0]._v = v;
				}
			}
			int64_t getBps(int64_t t)
			{
				int64_t v = 0;
				for (auto i = 0; i < NETIO_BPS_ITEMS; i++) {
					if (llabs(t - _tv[i]._t) > 1000)
						break;
					v += _tv[i]._v;
				}
				return v;
			}
		};
		class session
		{
		public:
			int _keyid;
			int _fd;
			int _fdlisten;
			int _status;
			int _protocol;
			int _readpause;
			int _msgtype;
			int _lastappmsg;//上次产生应用消息 0：没有； 1：产生
			uint32_t _udata;
			uint64_t _allsend;
			uint64_t _allrecv;
			int64_t _mstime_connected;
			ec::parsebuffer  _rbuf;
			ec::io_buffer<> _sndbuf;
			char _peerip[48];
			uint16_t _peerport;
			uint32_t _epollevents;//epoll events
			time_t   _time_error; //延迟断开的开始时间
			t_bps   _bpsRcv; //接受秒流量
			t_bps   _bpsSnd; //发送秒流量
			size_t _lastsndbufsize;
		private:
			ssext_data* _pextdata; //application session extension data
		public:
			_USE_EC_OBJ_ALLOCATOR
				session(blk_alloctor<>* pblkallocator, int fd, int fdlisten = -1)
				: _keyid(fd)
				, _fd(fd)
				, _fdlisten(fdlisten)
				, _status(EC_AIO_FD_CONNECTING)
				, _protocol(EC_AIO_PROC_TCP)
				, _readpause(0)
				, _msgtype(EC_AIO_MSG_TCP)
				, _lastappmsg(0)
				, _udata(0)
				, _allsend(0)
				, _allrecv(0)
				, _mstime_connected(ec::mstime())
				, _sndbuf(EC_AIO_SNDBUF_MAXSIZE, pblkallocator)
				, _peerport(0)
				, _epollevents(0)
				, _time_error(0)
				, _lastsndbufsize(-1)
				, _pextdata(nullptr)
			{
				memset(_peerip, 0, sizeof(_peerip));
			}

			session(session&& v) noexcept
				: _rbuf(std::move(v._rbuf)), _sndbuf(std::move(v._sndbuf))
			{
				_keyid = v._keyid;
				_fd = v._fd;
				_fdlisten = v._fdlisten;
				_status = v._status;
				_protocol = v._protocol;
				_readpause = v._readpause;
				_msgtype = v._msgtype;
				_lastappmsg = v._lastappmsg;
				_udata = v._udata;
				_allsend = v._allsend;
				_allrecv = v._allrecv;
				_mstime_connected = v._mstime_connected;
				memcpy(_peerip, v._peerip, sizeof(_peerip));
				_peerport = v._peerport;
				_epollevents = v._epollevents;
				_pextdata = v._pextdata;
				_time_error = v._time_error;
				_lastsndbufsize = v._lastsndbufsize;

				v._fd = -1;
				v._status = 0;
				v._bpsRcv = _bpsRcv;
				v._bpsSnd = _bpsSnd;
				v._pextdata = nullptr;
				v._lastsndbufsize = -1;
				memset(v._peerip, 0, sizeof(v._peerip));
			}

			inline int64_t getRcvBps()
			{
				return _bpsRcv.getBps(ec::mstime());
			}

			inline int64_t getSndBps()
			{
				return _bpsSnd.getBps(ec::mstime());
			}

			void setextdata(ssext_data* pdata)
			{
				if (_pextdata)
					delete _pextdata;
				_pextdata = pdata;
			}

			template<class _ClsPtr>
			bool getextdata(const char* clsname, _ClsPtr& ptr)
			{
				if (!_pextdata || (clsname && stricmp(clsname, _pextdata->classname())))
					return false;
				ptr = (_ClsPtr)_pextdata;
				return true;
			}

			/*!
			\brief do receive bytes
			\param pdata [in] received byte stream
			\param size [in] received byte stream size(bytes)
			\param pmsgout [out] application layer message parsed from byte stream
			\return msgtype
				-1: error, need close
				0:  null message;
				>0: message type;
			*/
			virtual int onrecvbytes(const void* pdata, size_t size, ec::ilog* plog, ec::bytes* pmsgout)
			{
				pmsgout->clear();
				pmsgout->append(pdata, size);
				return pmsgout->empty() ? EC_AIO_MSG_NUL : _msgtype;
			};

			// return -1:error; or (int)size
			virtual int sendasyn(const void* pdata, size_t size, ec::ilog* plog)
			{
				return _sndbuf.append((const uint8_t*)pdata, size) ? (int)size : -1;
			}

			virtual ~session()
			{
				if (_pextdata) {
					delete _pextdata;
					_pextdata = nullptr;
				}
			}

			//get udp send buffer
			virtual udb_buffer_* getudpsndbuffer()
			{
				return nullptr;
			}
			virtual bool onSendCompleted() { return true; } //return false will disconnected
			virtual void setHttpDownFile(const char* sfile, long long pos, long long filelen) {};
			virtual bool hasSendJob() { return false; };
			virtual void onUdpSendCount(int64_t numfrms, int64_t numbytes) {};
			virtual int  msglevel() { return 0; } //优先级,每次ec::aio::doRecvBuffer()能处理的消息数
			/**
			 * @brief 断开前处理，用于websocket等断开发送控制码
			 * @param ncode 状态码
			 * @param pdata 附加数据
			 * @param datasize 附件数据长度
			 * @return true表示有输出到异步发送去。false表示无任何操作
			 */
			virtual bool onClose(int ncode, const void* pdata, size_t datasize) { return false; }
			virtual const char* ProtocolName(int nprotoocl) {
				const char* sr = "";
				switch (nprotoocl) {
				case EC_AIO_PROC_TCP:
					sr = "TCP";
					break;
				case EC_AIO_PROC_TLS:
					sr = "TLS";
					break;
				case EC_AIO_PROC_HTTP:
					sr = "HTTP";
					break;
				case EC_AIO_PROC_HTTPS:
					sr = "HTTPS";
					break;
				case EC_AIO_PROC_WS:
					sr = "WS";
					break;
				case EC_AIO_PROC_WSS:
					sr = "WSS";
					break;
				}
				return sr;
			}
		};

		typedef session* psession;
		struct kep_session {
			bool operator()(int32_t key, const psession& val)
			{
				return key == val->_keyid;
			}
		};
		struct del_session {
			void operator()(psession& val)
			{
				if (val) {
					delete val;
					val = nullptr;
				}
			}
		};
	}//namespace aio
}//namespace ec