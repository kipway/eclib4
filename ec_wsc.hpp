/*!
\file ec_wsc.hpp
  websocket client class

\author	jiangyong
\email  kipway@outlook.com
\date 2024.8.11

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include "ec_wssclient.h"

/*错误码*/
#define ECWSC_SUCCESS 0  //成功
#define ECWSC_FAILED (-1) //失败
#define ECWSC_ERRURL (-2) //URL错误
#define ECWSC_NONUTF8 (-3) //含有非UTF8字符
#define ECWSC_CLOSE (-4) //连接已关闭
#define ECWSC_CLOSETIMEOUT (-5) //因发送超时而断开。
#define ECWSC_CONNECTING (-6) //连接中，不可用

/*事件码*/
#define ECWSCEVT_MESSAGE 0 //接收到的消息
#define ECWSCEVT_OPENSUCCESS  1 //连接成功
#define ECWSCEVT_OPENFAILED 2 //连接失败
#define ECWSCEVT_CLOSE 3 //连接断开

/**
 * @brief 事件回调函数
 * @evtcode 事件码; ECWSCEVT_XXX
 * @msg 消息内容; evtcode == ECWSCEVT_MESSAGE时
 * @sizemsg 当msg有效时的消息长度(字节数)
 * @appParam 调用者的参数,原样回调,一般存放this指针
 * @remakr: evtcode != ECWSCEVT_MESSAGE时，msg如果有效，含有错误信息。
*/
typedef void (*onEcWscEvent)(int evtcode, const void* msg, size_t sizemsg, void* appParam);

namespace ec {
	class iWebsocketClient
	{
	public:
		virtual int iCreate(const char* sip, uint16_t uport, const char* srequrl, const char* shost, const char* sprotocol, onEcWscEvent funEvent, void* funEventParam) = 0;
		virtual int iOpen() = 0;
		virtual int iClose() = 0;
		virtual int iSend(const void* msg, size_t nsize) = 0;
		virtual void iRuntime(int millisecond) = 0;
		virtual void Release() = 0;
		virtual int igetPoll(struct pollfd* outpoll) = 0;
		virtual void iruntimePoll(int pollEvent) = 0;
		virtual void iEnablePing(int intervalSecond) = 0;
	};

	template<class CLSWSC = ec::wss_c>
	class cWebsocketClient : public CLSWSC, public iWebsocketClient
	{
	public:
		cWebsocketClient(ec::ilog* plog) : CLSWSC(plog) {
		}
		virtual ~cWebsocketClient(){
		}
	protected:
		onEcWscEvent _funEvent = nullptr;
		void* _funEventParam = 0;
		int64_t _mstimeLastSend = 0;
		ec::string _sip;
		uint16_t _uport = 0;
		int _intervalSecond = 15;
	protected:
		void onwshandshake() override
		{
			_mstimeLastSend = ec::mstime();
			if (_funEvent) {
				_funEvent(ECWSCEVT_OPENSUCCESS, nullptr, 0, _funEventParam);
			}
		};
		int onwsdata(const uint8_t* p, int nbytes) override //return 0:OK ; -1:error will disconnect;
		{
			if (_funEvent) {
				_funEvent(ECWSCEVT_MESSAGE, p, nbytes, _funEventParam);
			}
			return 0;
		}

		void onconnectfailed() override
		{
			if (_funEvent) {
				_funEvent(ECWSCEVT_OPENFAILED, nullptr, 0, _funEventParam);
			}
		}
		void ondisconnected() override
		{
			CLSWSC::ondisconnected();
			if (_funEvent) {
				_funEvent(ECWSCEVT_CLOSE, nullptr, 0, _funEventParam);
			}
		}

	public:
		int iCreate(const char* sip, uint16_t uport, const char* srequrl, const char* shost, const char* sprotocol,
			onEcWscEvent funEvent, void* funEventParam) override
		{
			_sip = sip;
			_uport = uport;
			CLSWSC::initws(srequrl, shost, sprotocol);
			_funEvent = funEvent;
			_funEventParam = funEventParam;
			return 0;
		}

		int iOpen() override
		{
			if (CLSWSC::_status != ec::tcp_c::st_invalid)
				return ECWSC_SUCCESS;
			return this->open(_sip.c_str(), _uport) ? ECWSC_SUCCESS : ECWSC_FAILED;
		}

		int iClose() override
		{
			if (CLSWSC::_status == ec::tcp_c::st_invalid)
				return ECWSC_CLOSE;
			this->close(0);
			return 0;
		}

		int iSend(const void* msg, size_t nsize) override
		{
			if (CLSWSC::_status == ec::tcp_c::st_invalid)
				return ECWSC_CLOSE;
			if (!CLSWSC::get_ws_status())
				return ECWSC_CONNECTING;
			if (!ec::strisutf8((const char*)msg, nsize))
				return ECWSC_NONUTF8;
			int ns = CLSWSC::sendbytes(msg, (int)nsize);
			if (ns < 0)
				return ECWSC_CLOSE;
			_mstimeLastSend = ec::mstime();
			return ECWSC_SUCCESS;
		}

		void iRuntime(int millisecond) override
		{
			this->runtime(millisecond);
			if (_intervalSecond && CLSWSC::get_ws_status()) {
				int64_t mscur = ec::mstime();
				if (llabs(mscur - _mstimeLastSend) > _intervalSecond * 1000ll) {
					_mstimeLastSend = mscur;
					CLSWSC::sendPingMsg("heartline");
				}
			}
		}

		void Release() override
		{
			delete this;
		}

		int igetPoll(struct pollfd* outpoll) override
		{
			if (!outpoll)
				return -1;
			return this->getPoll(*outpoll);
		}

		void iruntimePoll(int pollEvent) override
		{
			this->runtimePoll(pollEvent);
			if (_intervalSecond && CLSWSC::get_ws_status()) {
				int64_t mscur = ec::mstime();
				if (llabs(mscur - _mstimeLastSend) > _intervalSecond * 1000ll) {
					_mstimeLastSend = mscur;
					CLSWSC::sendPingMsg("heartline");
				}
			}
		}

		void iEnablePing(int intervalSecond) override 
		{
			if (intervalSecond <= 0) {
				_intervalSecond = 0;
			}
			else {
				_intervalSecond = intervalSecond;
			}
		}
	};

	inline iWebsocketClient* CreateWebsocketClient(const char* wsurl, const char* protocols, ec::ilog* plog, onEcWscEvent funEvent, void* funParam, int* pRetCode)
	{
		if (!wsurl) {
			if (pRetCode)
				*pRetCode = ECWSC_FAILED;
			return nullptr;
		}
		ec::net::url<ec::string> _url;
		if (!_url.parse(wsurl, strlen(wsurl))) {
			if (pRetCode)
				*pRetCode = ECWSC_ERRURL;
			return nullptr;
		}
		if (_url._protocol.empty()) {
			if (pRetCode)
				*pRetCode = ECWSC_ERRURL;
			return nullptr;
		}
		iWebsocketClient* _pcls = nullptr;
		if (ec::strieq("ws", _url._protocol.c_str())) {
			if (!_url._port)
				_url._port = 80;
			_pcls = new cWebsocketClient<ec::ws_c>(plog);
			_pcls->iCreate(_url.ipstr(), _url._port, _url._path.c_str(), _url._host.c_str(), protocols, funEvent, funParam);
			if (pRetCode)
				*pRetCode = ECWSC_SUCCESS;
			return _pcls;
		}
		else if (ec::strieq("wss", _url._protocol.c_str())) {
			if (!_url._port)
				_url._port = 443;
			_pcls = new cWebsocketClient<ec::wss_c>(plog);
			_pcls->iCreate(_url.ipstr(), _url._port, _url._path.c_str(), _url._host.c_str(), protocols, funEvent, funParam);
			if (pRetCode)
				*pRetCode = ECWSC_SUCCESS;
			return _pcls;
		}
		if (pRetCode)
			*pRetCode = ECWSC_ERRURL;
		return nullptr;
	}
}