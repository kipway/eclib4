/*!
\file ec_aiohttps.h

Asynchronous https/wss session

\author  jiangyong
\update
  2024-5-8 添加发送websocket协议断开握手控制帧。
  2023-12-13 增加会话连接消息处理均衡
  2023-5-21 update for http download big file

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include "ec_aiohttp.h"
#include "ec_aiotls.h"

namespace ec {
	namespace aio {
		class session_https : public session_tls, public basews
		{
		protected:
			int _sendcloseFrame = 0;// 0:发送关闭控制帧标识，防止循环
			long long _downpos; //下载文件位置
			long long _sizefile;//文件总长度
			ec::string _downfilename;
		public:
			session_https(session_tls&& ss) : session_tls(std::move(ss)), _downpos(0), _sizefile(0)
			{
				_protocol = EC_AIO_PROC_HTTPS;
			}
		protected:
			virtual void onupdatews() {
				_protocol = EC_AIO_PROC_WSS;
			}
			virtual int session_send(const void* pdata, size_t size, ec::ilog* plog) {
				return session_tls::sendasyn(pdata, size, plog);
			}
		public:
			/**
			 * @brief 断开前处理，用于websocket等断开发送控制码
			 * @param ncode 状态码
			 * @param pdata 附加数据
			 * @param datasize 附件数据长度
			 * @return true表示有输出到异步发送去。false表示无任何操作
			 */
			virtual bool onClose(int ncode, const void* pdata, size_t datasize)
			{
				if (EC_AIO_PROC_WSS != _protocol || _sendcloseFrame)
					return false;
				_sendcloseFrame = 1;
				uint8_t data[8];
				data[0] = static_cast<uint8_t>((ncode & 0xFF00) >> 8);
				data[1] = static_cast<uint8_t>(ncode & 0xFF);
				return ws_send(_fd, data, 2, nullptr, WS_OP_CLOSE) > 0;
			}
			virtual int onrecvbytes(const void* pdata, size_t size, ec::ilog* plog, ec::bytes* pmsgout)
			{
				int nr = 0;
				pmsgout->clear();
				if (pdata && size) {
					nr = session_tls::onrecvbytes(pdata, size, plog, pmsgout);
					if (EC_AIO_MSG_TCP != nr)
						return nr;
				}
				_lastappmsg = 0;
				nr = DoReadData(_fd, (const char*)pmsgout->data(), pmsgout->size(), pmsgout, plog, _rbuf);
				if (he_failed == nr)
					return EC_AIO_MSG_ERR;
				else if (he_close == nr)
					return EC_AIO_MSG_CLOSE;
				else if (he_ok == nr) {
					if (PROTOCOL_HTTP == _nws) {
						_lastappmsg = 1;
						return EC_AIO_MSG_HTTP;
					}
					else if (PROTOCOL_WS == _nws) {
						_lastappmsg = 1;
						return EC_AIO_MSG_WS;
					}
					return EC_AIO_MSG_NUL;
				}
				return EC_AIO_MSG_NUL; //wait
			};

			// return -1:error; or (int)size
			virtual int sendasyn(const void* pdata, size_t size, ec::ilog* plog)
			{
				return ws_send(_fd, pdata, size, plog);
			}
			virtual bool onSendCompleted() //return false will disconnected
			{
				if (_protocol != EC_AIO_PROC_HTTPS || !_sizefile || _downfilename.empty())
					return true;
				if (_downpos >= _sizefile) {
					_downpos = 0;
					_sizefile = 0;
					_downfilename.clear();
					return true;
				}
				ec::string sbuf;
#ifdef _MEM_TINY
				long long lread = 1024 * 30;
#else
				long long lread = 1024 * 120;
#endif
				if (_downpos + lread > _sizefile)
					lread = _sizefile - _downpos;
				if (!io::lckread(_downfilename.c_str(), &sbuf, _downpos, lread, _sizefile))
					return false;
				if (sbuf.empty()) {
					_downpos = 0;
					_sizefile = 0;
					_downfilename.clear();
					return true;
				}
				_downpos += (long long)sbuf.size();
				if (_downpos >= _sizefile) {
					_downpos = 0;
					_sizefile = 0;
					_downfilename.clear();
				}
				return session_tls::sendasyn(sbuf.data(), sbuf.size(), nullptr) >= 0;
			}

			virtual void setHttpDownFile(const char* sfile, long long pos, long long filelen)
			{
				if (sfile && *sfile)
					_downfilename = sfile;
				else
					_downfilename.clear();
				_downpos = pos;
				_sizefile = filelen;
			}

			virtual bool hasSendJob() {
				return _sizefile && _downfilename.size();
			};
		};
	}// namespace aio
}// namespace ec
