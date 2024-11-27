/*!
 * @file ec_server.hpp
 * 更简单的后台服务程序框架,兼容 -std=c++0x, 单hpp文件无其他依赖。用于替代ec_service.h
 * 
 * 4大功能:
 *  1)单实例，始终只有一个进程运行。
 *  2)后台运行, 启动时创建子进程后台运行，父进程退出。
 *  3)自我管理, 支持命令行单参数管理， -ver, -status, -start, -stop, -kill , 其他参数或者无参数为应用程序自己参数，按照启动模式运行。
 *  4)系统服务，支持windows Service和Linux systemd(fork模式，指定PIDFile)
 *
 * 注：Windows下使用全局命令空间创建互拆量和内存映射文件，需要SeCreateGlobalPrivilege特权运行。
 *    默认的administrators, services和local system账号具有该权限. 也可以单独的给用户增加Create global objects权限。 
 *    Linux支持root和普通权限用户运行, 在编写systemd service文件时，先插入 -start参数，之后才是应用程序自己的参数, 参阅如下例子
 
 假如有一个带参数的服务 demohttpd 可以带一个参数 -port=1080
 
 Linux 用法：
 1) Fork到后台，前台退出，自己管理进程.
	demohttpd -start -port=1080

 2) 前台直接运行，不fork，适合其他进程管理，带单实例保护，可用于agent拉起proxy模式。
	demohttpd -run -port=1080

 3) systemd的后台服务模式service文件，启动命令行和Fork模式完全相同。

[Unit]
Description=demohttpd daemon
After=basic.target network.target

[Service]
WorkingDirectory=/usr/local/bin/demohttp
ExecStart=/usr/local/bin/demohttp/demohttpd -start -port=1080
ExecStop=/usr/local/bin/demohttp/demohttpd -stop

Type=forking
PIDFile=/var/tmp/demohttpd.pid
GuessMainPID=no
Restart=on-failure
RestartSec= 60
TimeoutStartSec=300

[Install]
WantedBy=multi-user.target


window用法
1) Fork到后台，前台退出，自己管理进程.
	demohttpd -start -port=1080

 2) 前台直接运行，不fork，适合其他进程管理，带单实例保护，可用于agent拉起proxy模式。
	demohttpd -run -port=1080

 3) 安装为service后台服务。
	demohttpd -install -port=1080

 4) 卸载后台服务
   demohttpd -uninstall

 * 
 * @author jiangyong
 * 更新记录:
 *   2024-11-25 使用ec::string替换std::string
 *   2024-8-19 使用signal(SIGPIPE, SIG_IGN);忽略SIGPIPE信号
 * 	 2024-4-29 增加windows网络库初始化
 *   2024-1-22 增加DebugRun()用于纯应用层功能调试。Linux启动改为和Windows一致，都需要-start参数。
 *   2024-1-19 由编译分别支持windows service和console改为同时支持, 当已安装为服务后，-start 等命令提示使用服务管理器.
 *   2024-1-18 windows service由subsystem:windows改为更简单subsystem:console.
 *   2023-1-17 首次发布。
 
eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once

#ifdef _WIN32
#include <tchar.h>
#include <tlhelp32.h>
#include <Psapi.h>
#include <winternl.h>

#else
#include <mntent.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/statfs.h>
#include <sys/time.h>
#include<sys/ipc.h> 
#include<sys/msg.h> 
#include <termios.h>
#endif

#include <signal.h>
#include <cstring>
#include <string>
#include <thread>
#include <cstdint>
#include <vector>
#include "ec_vector.hpp"
#include "ec_string.hpp"

#if defined _WIN32
void WINAPI g_ServiceMain(DWORD dwArgc, LPTSTR* lpszArgv);
void WINAPI g_ServiceHandler(DWORD dwOpcode);
BOOL WINAPI exit_HandlerRoutine(DWORD dwCtrlType);

#define DECLARE_APP_MAIN(CLSAPP)\
void WINAPI g_ServiceMain(DWORD dwArgc, LPTSTR* lpszArgv)\
{\
	CLSAPP.ServiceMain(dwArgc, lpszArgv);\
};\
void WINAPI g_ServiceHandler(DWORD dwOpcode)\
{\
	CLSAPP.ServiceHandler(dwOpcode);\
}\
\
int main(int argc, const char** argv)\
{\
	if(1 == argc)\
		CLSAPP.Usage();\
	else {\
		if (!strcmp("-ver", argv[1]) || !strcmp("-verson", argv[1])) {\
			printf("%s %s %s\n", CLSAPP.getInstName(), CLSAPP.version(), CLSAPP.buildinfo());\
		}\
		else if (!strcmp("-status", argv[1])) {\
			CLSAPP.Status();\
		}\
		else if (!strcmp("-install", argv[1])) {\
			CLSAPP.RegisterServer(argc, argv);\
		}\
		else if (!strcmp("-uninstall", argv[1])) {\
			CLSAPP.UnregisterServer(); \
		}\
		else if (!strcmp("-service", argv[1])) {\
			CLSAPP.ServiceStart(argc, argv);\
		}\
		else if (!strcmp("-stop", argv[1])) {\
			CLSAPP.Stop(0);\
		}\
		else if (!strcmp("-kill", argv[1])) {\
			CLSAPP.Stop(1);\
		}\
		else if (!strcmp("-help", argv[1])) {\
			CLSAPP.Usage();\
		}\
		else\
			CLSAPP.Start(argc, argv);\
	}\
	return 0;\
}\
BOOL WINAPI exit_HandlerRoutine(DWORD dwCtrlType) {\
	printf("dwCtrlType %u\n", dwCtrlType);\
	switch (dwCtrlType) {\
	case CTRL_C_EVENT: \
	case CTRL_BREAK_EVENT:\
	case CTRL_CLOSE_EVENT: \
	case CTRL_SHUTDOWN_EVENT: \
		CLSAPP.StopRun((int)dwCtrlType);\
		break;\
	default:\
		return FALSE;\
	}\
	return TRUE;\
}\

#else // Linux 版
void exit_handler(int sigval);

#define DECLARE_APP_MAIN(CLSAPP)\
void exit_handler(int sigval)\
{\
	if (sigval) {\
		signal(sigval, SIG_IGN);\
	}\
	CLSAPP.StopRun(sigval);\
}\
\
int main(int argc, const char** argv)\
{\
	if (2 == argc) {\
		if (!strcmp("-status", argv[1])) {\
			CLSAPP.Status();\
		}\
		else if (!strcmp("-ver", argv[1]) || !strcmp("-verson", argv[1])) {\
			printf("%s %s\n", CLSAPP.version(), CLSAPP.buildinfo());\
		}\
		else if (!strcmp("-stop", argv[1])) {\
			CLSAPP.Stop(0);\
		}\
		else if (!strcmp("-kill", argv[1])) {\
			CLSAPP.Stop(1);\
		}\
		else if (!strcmp("-help", argv[1])) {\
			CLSAPP.Usage();\
		}\
		else\
			CLSAPP.Start(argc, argv);\
	}\
	else {\
		CLSAPP.Start(argc, argv);\
	}\
	return 0;\
}

#endif

//windows版使用共享内存模拟消息队列，Linux版使用system V消息队列来协调子进程启动成功和停止成功。
#define _CTRLMAPBUF_PIDSIZE 64 //存放PID大小
#define _CTRLMAPBUF_ORDERSIZE  128 //存放命令大小
#define _CTRLMAPBUF_SIZE (_CTRLMAPBUF_PIDSIZE + _CTRLMAPBUF_ORDERSIZE + _CTRLMAPBUF_ORDERSIZE) //总大小, pid存放去，输入区，输出区（输入输出为子进程角度）
#define _CTRLMAPBUF_ORDERINPOS 64 //输入命令起始地址
#define _CTRLMAPBUF_ORDEROUTPOS 192 //输出命令起始地址

namespace ec {
	/**
	 * @brief 后台服务基类
	*/
	class CServerApp
	{
	protected:
		int _sigval; //终止信号
		ec::string _workapth; ///!工作目录
		ec::string _instname; ///!实例名/服务名
		ec::string _pidpathfile; ///!全路经的pid文件名
#if defined _WIN32
		SERVICE_STATUS_HANDLE	m_hServiceStatus;
		SERVICE_STATUS			m_status;
		char _serviceName[128];
		ec::vector<ec::string> _service_argv; //服务的命令行参数
		HANDLE _hMapFile; ///! 共享内存用于存储PID,子进程创建,子进程退出时关闭。
		HANDLE _hMutex; ///!windows使用命名互拆对象来判断一个驱动实例是否运行。
#else
		struct t_msg {
			long mtype;
			char mtext[_CTRLMAPBUF_SIZE];
		};
		int	m_nlockfile; ///!linux版使用pid文件锁来判断一个驱动实例是否运行, 后台服务需要使用。
		int _msgqueueid;///!主进程创建的临时消息队列ID，接收子进程发送的启动和停止信息, 主进程退出时，调用msgctrl清除队列，否则会在系统中存留。
#endif

	protected: //应用层需重载的启动，停止，运行三个函数, 参见例子 testserver.cpp
		/**
		 * @brief 子进程创建后调用，启动相当普通应用的于main函数
		 * @param argc 参数个数
		 * @param argv 参数数组, 这里已经去除了本系统自带'-start'等参数，属于纯应用自己的参数。例如应用带有一个参数 '-port=1080'
		 * 使用-start启动时，命令行参数为 -start -port=1080; 此时 argc=3,argv[0]是应用程序名,argv[1]='_start'; argv[2]='-port=1080'; 
		 * OnStart里会去除-start, 此时argc=2; argv[0]还是应用程序名, argv[1]='-port=1080'
		 * @return 0：成功; -1失败，退出
		*/
		virtual int OnStart(int argc, const char** argv) = 0;

		/**
		 * @brief 重载优雅退出时清理工作
		 * @param sigVal 退出信号,linux为 15， Windows为 2
		 * @return 0:优雅退出清理完成，-1：清理有错
		*/
		virtual int OnStop(int sigVal) = 0;

		/**
		 * @brief 应用运行时，会在RunLoop中被循环调用,应用程序不应该长时间阻塞这个调用。
		*/
		virtual void RunTime() = 0;

	public: //应用层需要重载的基本信息
		/**
		 * @brief 版本信息，应用层重载。
		 * @return 版本信息
		*/
		virtual const char* version()
		{
			return "ver 1.0.1";
		}

		/**
		 * @brief build信息，应用层重载
		 * @return build信息
		*/
		virtual const char* buildinfo()
		{
			return "build 2024-1-10";
		}

		/**
		 * @brief 描述信息, windows首台服务使用
		 * @return 
		*/
		virtual const char* description()
		{
			return "CServerApp description";
		}

		virtual const char* osinfo()
		{
#ifdef _WIN32
			return "Windows";
#else
			return "Linux";
#endif
		}
#ifdef _WIN32
		virtual void Usage()
		{
			printf("\n%s %s %s\n\nUsage:\n", _instname.c_str(), version(), buildinfo());
			printf("  -ver : show version information\n");
			printf("  -install : install as service. requires administrator power.\n");
			printf("  -uninstall : uninstall service. requires administrator power.\n");
			printf("  -status : Check running status\n");
			printf("  -run : direct run\n"); //直接运行,适合只要单实例功能，应用自己的命令行参数放在后面。
			printf("  -debug : debug run\n"); //调试模式运行,不附加任何功能，应用自己的命令行参数放在后面。
			printf("  -start : start and fork to background\n");
			printf("  -stop : stop run\n");
			printf("  -kill : force stop run\n");
			printf("  -help : view this information\n");
			if(isService(0))
				printf("\n%s has been installed as service.\n", _instname.c_str());
		}
		inline const char* getInstName() {
			return _instname.c_str();
		}

		inline void getMutexName(ec::string& so) const
		{
			so = "Global\\";
			so.append(_instname).append(".mutex");
		}

		inline void getMapFileName(ec::string& so) const
		{
			so = "Global\\";
			so.append(_instname).append(".mapf");
		}

		char* getServiceName() {
			return _serviceName;
		}

		BOOL CheckPrivilege()///!检查是否具备'SeCreateGlobalPrivilege'
		{
			BOOL hasPrivilege = FALSE;
			PRIVILEGE_SET prset;
			prset.PrivilegeCount = 1;
			prset.Control = PRIVILEGE_SET_ALL_NECESSARY;
			prset.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

			if (!LookupPrivilegeValue(NULL, SE_CREATE_GLOBAL_NAME, &prset.Privilege[0].Luid)) {
#ifdef _DEBUG
				printf("LookupPrivilegeValue failed.\n");
#endif
				return hasPrivilege;
			}
			HANDLE hToken = NULL;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
#ifdef _DEBUG
				printf("OpenProcessToken failed.\n");
#endif
				return hasPrivilege;
			}

			if (!PrivilegeCheck(hToken, &prset, &hasPrivilege))
				hasPrivilege = FALSE;
			CloseHandle(hToken);
			if(!hasPrivilege)
				printf("start failed. plear make sure you have 'SeCreateGlobalPrivilege'.\n");
			return hasPrivilege;
		}
#else
		/**
		 * @brief 帮助用法，重载输出应用帮助。
		*/
		virtual void Usage()
		{
			printf("\n%s %s %s\n\nUsage:\n", _instname.c_str(), version(), buildinfo());
			printf("  -ver : Show version information\n");
			printf("  -status : Check running status\n");
			printf("  -start : start and fork to background. optional, not required.\n"); //启动后台运行，-start参数可选，不是必须的。
			printf("  -run : direct run\n"); //直接运行,适合只要单实例功能，应用自己的命令行参数放在后面。
			printf("  -debug : debug run\n"); //调试模式运行,不附加任何功能，应用自己的命令行参数放在后面。
			printf("  -stop : Stop run in the background\n");
			printf("  -kill : force stop run in the background\n");
			printf("  -help : view this information\n");
		}
#endif
		
	public:
		CServerApp() :
			_sigval(-1)
		{
#ifdef _WIN32
			m_hServiceStatus = NULL;
			m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
			m_status.dwCurrentState = SERVICE_STOPPED;
			m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
			m_status.dwWin32ExitCode = 0;
			m_status.dwServiceSpecificExitCode = 0;
			m_status.dwCheckPoint = 0;
			m_status.dwWaitHint = 0;
			memset(_serviceName, 0, sizeof(_serviceName));

			_hMapFile = NULL;
			_hMutex = NULL;
			WSADATA wsaData;// Initialize Winsock
			int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
			if (iResult != NO_ERROR) {
				printf("Error %d at WSAStartup", iResult);
		}
#else
			m_nlockfile = -1;
			_msgqueueid = -1;
#endif

		}

		virtual ~CServerApp()
		{
#if defined _WIN32
			if (_hMutex) {
				CloseHandle(_hMutex);
				_hMutex = NULL;
			}
			if (_hMapFile) {
				CloseHandle(_hMapFile);
				_hMapFile = NULL;
			}
			WSACleanup();
#else
			if (m_nlockfile >= 0) {
				close(m_nlockfile);
				m_nlockfile = -1;
			}
#endif
		}

#if defined _WIN32
		/**
		 * @brief  服务入口,服务模式不需要做单实例和消息通知，采用windows服务机制。
		 * @param dwArgc 参数个数
		 * @param lpszArgv 参数数组
		*/
		void ServiceMain(DWORD dwArgc, LPTSTR* lpszArgv)
		{
			m_status.dwCurrentState = SERVICE_START_PENDING;
			m_hServiceStatus = RegisterServiceCtrlHandler(_instname.c_str(), g_ServiceHandler);
			if (m_hServiceStatus == NULL)
				return;
			SetServiceStatus(SERVICE_START_PENDING);

			m_status.dwWin32ExitCode = S_OK;
			m_status.dwCheckPoint = 0;
			m_status.dwWaitHint = 0;

			ec::vector<const char*> argv;
			for (auto& i : _service_argv)
				argv.push_back(i.c_str());
			argv.push_back(NULL);
			if (0 != OnStart((int)_service_argv.size(), argv.data())) {
				SetServiceStatus(SERVICE_STOPPED);
				return; //启动失败
			}
			SetServiceStatus(SERVICE_RUNNING);
			while (-1 == _sigval) {
				RunTime();
			}
			OnStop(_sigval);
			SetServiceStatus(SERVICE_STOPPED);
		}

		void ServiceHandler(DWORD dwOpcode) //服务事件响应
		{
			switch (dwOpcode) {
			case SERVICE_CONTROL_STOP:
				SetServiceStatus(SERVICE_STOP_PENDING);
				_sigval = (int)dwOpcode;
				break;
			case SERVICE_CONTROL_PAUSE:
				break;
			case SERVICE_CONTROL_CONTINUE:
				break;
			case SERVICE_CONTROL_INTERROGATE:
				break;
			case SERVICE_CONTROL_SHUTDOWN:
				SetServiceStatus(SERVICE_STOP_PENDING);
				_sigval = (int)dwOpcode;
				break;
			default:
				break;
			}
		}

		void ServiceStart(int argc, const char** argv)///!启动服务
		{
			strcpy(_serviceName, _instname.c_str());
			int i;
			_service_argv.push_back(std::string(argv[0]));
			for (i = 2; i < argc; i++) {
				_service_argv.push_back(std::string(argv[i]));
			}
			SERVICE_TABLE_ENTRY st[] = {
				{ _serviceName, g_ServiceMain },
				{ NULL, NULL }
			};
			StartServiceCtrlDispatcher(st);
		};
		
		BOOL IsInstalled()
		{
			BOOL bResult = FALSE;
			SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
			if (hSCM != NULL) {
				SC_HANDLE hService = ::OpenService(hSCM, _instname.c_str(), SERVICE_QUERY_CONFIG);
				if (hService != NULL) {
					bResult = TRUE;
					::CloseServiceHandle(hService);
				}
				::CloseServiceHandle(hSCM);
			}
			else {
				printf("Couldn't open service manager.\nPlease make sure you have administrator power.\n");
			}
			return bResult;
		}

		BOOL isService(int viewinfo = 1)
		{
			BOOL bResult = FALSE;
			SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
			if (hSCM != NULL) {
				SC_HANDLE hService = ::OpenService(hSCM, _instname.c_str(), SERVICE_QUERY_STATUS);
				if (hService != NULL) {
					bResult = TRUE;
					::CloseServiceHandle(hService);
				}
				::CloseServiceHandle(hSCM);
			}
			if(bResult && viewinfo)
				printf("%s has been install as service. Please use windows service manager.\n", _instname.c_str());
			return bResult;
		}

		/**
		 * @brief 安装服务并添加故障重启,需要administrators组权限
		 * @return 非零成功，0：失败；
		*/
		BOOL Install(int argc , const char** argv)
		{
			if (IsInstalled())
				return TRUE;
			SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
			if (hSCM == NULL) {
				printf("Couldn't open service manager.\nPlease make sure you have administrator power.\n");
				return FALSE;
			}
			char szFilePath[_MAX_PATH];
			::GetModuleFileName(NULL, szFilePath, _MAX_PATH);

			char szBinfile[_MAX_PATH];
			strcpy(szBinfile, szFilePath);
			strcat(szBinfile, " -service");

			int i;
			for (i = 2; i < argc; i++) { //跳过 '-install'
				strcat(szBinfile," ");
				strcat(szBinfile, argv[i]);
			}

			SC_HANDLE hService = ::CreateService(
				hSCM, _instname.c_str(), _instname.c_str(),
				GENERIC_ALL, SERVICE_WIN32_OWN_PROCESS,
				SERVICE_AUTO_START,
				SERVICE_ERROR_IGNORE,
				szBinfile, NULL, NULL, NULL, NULL, NULL);

			if (hService == NULL) {
				::CloseServiceHandle(hSCM);
				printf("Couldn't create service %s\n", _instname.c_str());
				return FALSE;
			}

			SC_ACTION  Actions; //添加故障重启
			Actions.Type = SC_ACTION_RESTART;
			Actions.Delay = 60 * 1000; //1 minute

			SERVICE_FAILURE_ACTIONS act;
			memset(&act, 0, sizeof(act));
			act.dwResetPeriod = 0;
			act.lpRebootMsg = nullptr;
			act.lpCommand = nullptr;
			act.cActions = 1;
			act.lpsaActions = &Actions;

			if (!ChangeServiceConfig2(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &act))
				printf("Configuration failure recovery failed!\nplease manually configure service failure recovery!\n");

			char sKey[_MAX_PATH];
			strcpy(sKey, "SYSTEM\\CurrentControlSet\\Services\\");
			strcat(sKey, _instname.c_str());

			HKEY hkey;
			if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, sKey, 0, KEY_WRITE | KEY_READ, &hkey) == ERROR_SUCCESS) {
				RegSetValueEx(hkey, "Description", NULL, REG_SZ, (LPBYTE)description(), (DWORD)strlen(description()) + 1);
				RegCloseKey(hkey);
			}

			::CloseServiceHandle(hService);
			::CloseServiceHandle(hSCM);

			printf("install service %s success!\n", _instname.c_str());
			return TRUE;
		}
		BOOL Uninstall() //卸载，仅删除注册表信息,需要administrators组权限
		{
			if (!IsInstalled())
				return TRUE;
			SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
			if (hSCM == NULL) {
				printf("Couldn't open service manager.\nPlease make sure you have administrator power.\n");
				return FALSE;
			}
			SC_HANDLE hService = ::OpenService(hSCM, _instname.c_str(), SERVICE_STOP | DELETE);
			if (hService == NULL) {
				::CloseServiceHandle(hSCM);
				printf("Couldn't open service %s\n", _instname.c_str());
				return FALSE;
			}
			SERVICE_STATUS status;
			::ControlService(hService, SERVICE_CONTROL_STOP, &status);

			BOOL bDelete = ::DeleteService(hService);
			::CloseServiceHandle(hService);
			::CloseServiceHandle(hSCM);
			if (bDelete) {
				printf("uninstall service %s success!\n", _instname.c_str());
				return TRUE;
			}
			printf("uninstall service %s failed.\n", _instname.c_str());
			return FALSE;
		}
		
		void SetServiceStatus(DWORD dwState)
		{
			m_status.dwCurrentState = dwState;
			::SetServiceStatus(m_hServiceStatus, &m_status);
		}

		BOOL RegisterServer(int argc, const char** argv)///!先卸载，再安装，需要administrators组权限
		{
			Uninstall();
			return Install(argc, argv);
		}
		inline BOOL UnregisterServer() ///!卸载服务，需要administrators组权限
		{
			return Uninstall();
		}

		/**
		 * @brief 获取实例的PID,PID存贮在共享内存头部。
		 * @return 0:没有运行; >0 进程的PID;  -1：错误
		*/
		int getProcessID()
		{
			ec::string smutex, smapfile;
			getMutexName(smutex);
			getMapFileName(smapfile);
			
			HANDLE hMutex = OpenMutex(SYNCHRONIZE, 0, smutex.c_str());
			if (!hMutex) {
				DWORD dwErr = GetLastError();
				if (ERROR_FILE_NOT_FOUND == dwErr)
					return 0; //不存在
				else if (ERROR_ACCESS_DENIED) {
					printf("OpenMutex %s ERROR_ACCESS_DENIED\n", smutex.c_str());
				}
				else
					printf("OpenMutex %s GetLastError %u\n", smutex.c_str(), dwErr);
				return -1;
			}
			HANDLE hMapFile = OpenFileMapping(FILE_MAP_READ, FALSE, smapfile.c_str());
			if (!hMapFile) {
				CloseHandle(hMutex);
				printf("OpenFileMapping %s failed. GetLastError()=%u\n", smapfile.c_str(), GetLastError());
				return -1;
			}
			char* pBuf = (char*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, 0);
			if (!pBuf) {
				CloseHandle(hMapFile);
				CloseHandle(hMutex);
				return -1;
			}

			int nloop = 10, npid = 0;
			DWORD dwst = 0;
			while (nloop) {
				dwst = WaitForSingleObject(hMutex, 1000);
				if (WAIT_OBJECT_0 == dwst) {  // 获取互斥量
					npid = atoi(pBuf);
					ReleaseMutex(hMutex); //释放互斥量
					break;
				}
				else if (WAIT_FAILED == dwst) {
					printf("WaitForSingleObject GetLastError %u\n", GetLastError());
				}
				--nloop;
			}
			UnmapViewOfFile(pBuf);
			CloseHandle(hMapFile);
			CloseHandle(hMutex);
			if (!nloop)
				return -1;
			return npid;
		}

		void KillPID(int npid)
		{
			HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, npid);
			if (!hProcess) {
				printf("OpenProcess pid=%d failed!\n", npid);
				return;
			}
			TerminateProcess(hProcess, 9); //强制退出
			CloseHandle(hProcess);
		}

		/**
		 * @brief 创建消息队列，子进程用，windows使用共享内存模拟
		 * @return 0:success; -1:failed; 1:已经存在
		*/
		int CreateMessageQueue()
		{
			ec::string smutex, smapfile;
			getMutexName(smutex);
			getMapFileName(smapfile);

			//先创建内存映射文件,后创建信号量，和getProcessID顺序相反
			_hMapFile = CreateFileMapping(
				INVALID_HANDLE_VALUE,// use paging file
				NULL,                // default security
				PAGE_READWRITE,      // read/write access
				0,                   // maximum object size (high-order DWORD)
				_CTRLMAPBUF_SIZE,    // maximum object size (low-order DWORD)
				smapfile.c_str());
			if (!_hMapFile) {
				printf("CreateFileMapping failed GetLastError %u\n", GetLastError());
				return -1;
			}
			if (GetLastError() == ERROR_ALREADY_EXISTS) {
				printf("Memory mapping %s alreay exists!\n", smapfile.c_str());
				return 1; //已存在
			}

			_hMutex = CreateMutex(nullptr, FALSE, smutex.c_str()); //再创建信号量
			if (!_hMutex) {
				printf("CreateMutex failed!\n");
				return -1;
			}
			if (GetLastError() == ERROR_ALREADY_EXISTS) {
				printf("Mutex %s alreay exists!\n", smutex.c_str());
				return 1; //已存在
			}
			// 映射共享内存
			char* pBuf = (char*)MapViewOfFile(_hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
			if (pBuf == NULL) {
				printf("map share memory failed!\n");
				return -1;
			}
			int nloop = 10;
			while (nloop) {
				if (WAIT_OBJECT_0 == WaitForSingleObject(_hMutex, 1000)) {  // 获取互斥量
					memset(pBuf, 0, _CTRLMAPBUF_SIZE);
					snprintf(pBuf, _CTRLMAPBUF_SIZE / 2, "%u", GetCurrentProcessId());
					ReleaseMutex(_hMutex);  // 释放互斥量
					break;
				}
				--nloop;
			}
			UnmapViewOfFile(pBuf);
			if (!nloop) {
				printf("Write memory mapping file %s failed.\n", smapfile.c_str());
				return -1;
			}
			return 0;
		}

		/**
		 * @brief 打开消息队列，主进程用
		 * @return 0:success; -1:failed
		*/
		int OpenMessageQueue()
		{
			if (_hMutex && _hMapFile)
				return 0;
			ec::string smutex, smapfile;
			getMutexName(smutex);
			getMapFileName(smapfile);

			_hMutex = OpenMutex(SYNCHRONIZE, 0, smutex.c_str());
			if (!_hMutex) {
				return -1;
			}

			_hMapFile = OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, smapfile.c_str());
			if (!_hMapFile) {
				CloseHandle(_hMutex);
				_hMutex = NULL;
				return -1;
			}
			return 0;
		}

		/**
		 * @brief 写消息，双方都可使用
		 * @param sorder 命令字符串
		 * @param pos 消息位置,主进程和子进程pos参数不一样。
		 * @return 0:success; -1:error;
		*/
		int WirteMessage(const char* sorder, int pos)
		{
			if (!_hMapFile || !_hMutex || !sorder || strlen(sorder) >= _CTRLMAPBUF_ORDERSIZE
				|| pos + _CTRLMAPBUF_ORDERSIZE > _CTRLMAPBUF_SIZE)
				return -1;
			char* pBuf = (char*)MapViewOfFile(_hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
			if (pBuf == NULL) {
				return -1;
			}
			int nloop = 10;
			char* pOrder = pBuf + pos;
			while (nloop) {
				if (WAIT_OBJECT_0 == WaitForSingleObject(_hMutex, 1000)) {  // 获取互斥量
					strcpy(pOrder, sorder);
					ReleaseMutex(_hMutex);  // 释放互斥量
					break;
				}
				--nloop;
			}
			UnmapViewOfFile(pBuf);
			return nloop ? 0 : -1;
		}

		/**
		 * @brief 读消息
		 * @param so 读回的消息
		 * @param pos 消息位置,主进程和子进程pos参数不一样。
		 * @return -1:error; >=0 :读到的消息的字符数
		*/
		int ReadMessage(std::string& so, int pos)
		{
			so.clear();
			if (!_hMapFile || !_hMutex || pos + _CTRLMAPBUF_ORDERSIZE > _CTRLMAPBUF_SIZE)
				return -1;
			char* pBuf = (char*)MapViewOfFile(_hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
			if (pBuf == NULL) {
				return -1;
			}
			int nloop = 10;
			char* pOrder = pBuf + pos;
			while (nloop) {
				if (WAIT_OBJECT_0 == WaitForSingleObject(_hMutex, 1000)) {  // 获取互斥量
					if (*pOrder) {
						pOrder[_CTRLMAPBUF_ORDERSIZE - 1] = 0;
						so.append(pOrder);
						*pOrder = 0; //读后清除,所以前面要有 FILE_MAP_WRITE
					}
					ReleaseMutex(_hMutex);  // 释放互斥量
					break;
				}
				--nloop;
			}
			UnmapViewOfFile(pBuf);
			return nloop ? (int)so.size() : -1;
		}

#else  // Linux
		/**
		 * @brief  主进程创建消息队列,使用system V的消息队列
		 * @return 0:success; -1:failed; 1:已经存在
		*/
		int CreateMessageQueue()
		{			
			int nkey = ftok(_pidpathfile.c_str(), 1);//nkey不会变
			if (nkey < 0) {
				printf("ftok(%s, 1) failed.\n", _pidpathfile.c_str());
				return -1;
			}
			_msgqueueid = msgget(nkey, IPC_CREAT | 0644);//销毁再次创建 queueid会改变.
			if (_msgqueueid < 0) {
				if (EEXIST == errno)
					printf("create msg is exists!\n");
				else if (38 == errno) {
					printf("your system not support systemV message queue, it's WSL?\n");
					return -1;
				}
				else {
					printf("create systemV message queue errno %d, nkey = %d\n", errno, nkey);
				}
				return -1;
			}
#ifdef _DEBUG
			printf("create systemV message queue success. msgqueueid = %d, nkey = %d\n", _msgqueueid, nkey);
#endif
			return 0;
		}

		/**
		 * @brief 主进程打开消息队列
		 * @return 0:success; -1:failed;
		*/
		int OpenMessageQueue()
		{
			int nkey = ftok(_pidpathfile.c_str(), 1);
			if (nkey < 0) {
				_msgqueueid = -1;
				printf("ftok(%s, 1) failed.\n", _pidpathfile.c_str());
				return -1;
			}
#ifdef _DEBUG
			printf("ftok(%s,1) = %d\n", _pidpathfile.c_str(), nkey);
#endif
			_msgqueueid = msgget(nkey, IPC_EXCL | 0644);
			if (_msgqueueid < 0) {
				printf("OpenMessageQueue failed errno %d! nkey=%d\n", errno, nkey);
				return -1;
			}
			return 0;
		}

		/**
		 * @brief 写消息
		 * @param smsg 消息类容
		 * @param pos 忽略
		 * @return 0:success; -1:failed
		*/
		int WirteMessage(const char* smsg, int pos)
		{
			if (_msgqueueid < 0 || !smsg || !*smsg)
				return -1;
			size_t zlen = strlen(smsg) + 1;

			t_msg msg;
			msg.mtype = 1;
			if (zlen > sizeof(msg.mtext))
				return -1;
			memcpy(msg.mtext, smsg, zlen);
			int nr = msgsnd(_msgqueueid, &msg, zlen, IPC_NOWAIT);
			if (-1 == nr)
				return errno != EAGAIN ? -1 : 0;
			return nr;
		}

		/**
		 * @brief 读消息
		 * @param so 输出消息对象，先会清空, 复制模式
		 * @param pos 该参数被忽略
		 * @return -1:error; >=0 :读到的消息的字符数
		*/
		int ReadMessage(std::string& so, int pos)
		{
			so.clear();
			if (_msgqueueid < 0)
				return -1;
			t_msg msg;
			ssize_t szr = msgrcv(_msgqueueid, &msg, sizeof(msg.mtext), 0, IPC_NOWAIT);
			if (szr < 0)
				return 0;
			so = msg.mtext;
			return (int)so.size();
		}


		inline void KillPID(int npid)
		{
			kill(npid, 9);
		}

		/**
		 * @brief 读取进程PID
		 * @return 0:无； -1:失败; >0 存在的PID
		*/
		int getProcessID()
		{
			int nfile = open(_pidpathfile.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			if (nfile < 0)
				return -1;
			struct flock fl;
			fl.l_start = 0;
			fl.l_whence = SEEK_SET;
			fl.l_len = 0;
			fl.l_type = F_WRLCK;
			fl.l_pid = -1;
			if (fcntl(nfile, F_GETLK, &fl) < 0) {
				close(nfile);
				return -1;
			}
			if (fl.l_type == F_UNLCK) {
				close(nfile);
				return 0;
			}
			if (fl.l_pid == -1) {
				char spid[80] = { 0 };
				if (read(nfile, spid, sizeof(spid)) > 0)
					fl.l_pid = atoi(spid);
				else
					fl.l_pid = -1;
			}
			close(nfile);
			return (int)fl.l_pid; // return the pid
		}

		/**
		 * @brief 检查是否锁定，如果没有就锁定
		 * @param sfile
		 * @return -1:error;  0: success, not lock; >0 : the pid locked
		*/
		int CheckLock()
		{
			m_nlockfile = open(_pidpathfile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			if (m_nlockfile < 0)
				return -1;
			struct flock fl;
			fl.l_start = 0;
			fl.l_whence = SEEK_SET;
			fl.l_len = 0;
			fl.l_type = F_WRLCK;
			fl.l_pid = -1;
			if (fcntl(m_nlockfile, F_GETLK, &fl) < 0)
				return -1;
			if (fl.l_type == F_UNLCK) // if unlock lock and return current pid
				return LockFile(m_nlockfile);
			if (fl.l_pid == -1) {
				char spid[80] = { 0 };
				if (read(m_nlockfile, spid, sizeof(spid)) > 0)
					fl.l_pid = atoi(spid);
				else
					fl.l_pid = -1;
			}
			return fl.l_pid; // return the pid
		}

		/**
		 * @brief 锁定并写入当前PID
		 * @param nfile 文件fd
		 * @return 0:success; -1:failed
		*/
		int LockFile(int nfile) //lock and write pid to file,return 0:success
		{
			char buf[32];
			struct flock fl;
			fl.l_start = 0;
			fl.l_whence = SEEK_SET;
			fl.l_len = 0;
			fl.l_type = F_WRLCK;
			fl.l_pid = getpid();
			if (fcntl(nfile, F_SETLKW, &fl) < 0) //Blocking lock
				return -1;
			if (ftruncate(nfile, 0))
				return -1;
			lseek(nfile, 0, SEEK_SET);
			sprintf(buf, "%ld\n", (long)getpid());
			if (write(nfile, buf, strlen(buf)) <= 0)
				return -1;
			return 0;
		}

		static void CloseIO()
		{
			int fd = open("/dev/null", O_RDWR);
			if (fd < 0)
				return;
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
#endif

		/**
		 * @brief 格式化目录，最后为 '/'
		 * @param path
		*/
		void formatPath(ec::string& path)
		{
			for (size_t i = 0; i < path.size(); ++i) {
				if(path[i] == '\\')
					path[i] = '/';
			}
			if (path.size() > 0 && path[path.size() - 1] != '/')
				path.push_back('/');
		}

		/**
		 * @brief 读取当前执行文件的路径，Windows下转换为UTF8编码
		 * @param spath
		 * @return 0:success; -1:failed
		*/
		int getAppPath(ec::string& spath)
		{
#ifdef _WIN32
			wchar_t sFilename[1024];
			wchar_t sDrive[_MAX_DRIVE];
			wchar_t sDir[_MAX_DIR];
			wchar_t sFname[_MAX_FNAME];
			wchar_t sExt[_MAX_EXT];
			spath.clear();
			GetModuleFileNameW(NULL, sFilename, sizeof(sFilename) / sizeof(wchar_t));
			_wsplitpath(sFilename, sDrive, sDir, sFname, sExt);

			char sdrv[8] = { 0 };
			char sutf8[_MAX_DIR * 3];
			sutf8[0] = 0;
			if (!WideCharToMultiByte(CP_UTF8, 0, sDrive, -1, sdrv, (int)sizeof(sdrv), NULL, NULL)
				|| !WideCharToMultiByte(CP_UTF8, 0, sDir, -1, sutf8, (int)sizeof(sutf8), NULL, NULL))
				return -1;
			spath = sdrv;
			spath += sutf8;
			for (auto& i : spath) {
				if (i == '\\')
					i = '/';
			}
			if (spath.back() != '/')
				spath.push_back('/');
#else
			char sopath[1024] = { 0 };
			int n = readlink("/proc/self/exe", sopath, sizeof(sopath) - 1);
			if (n < 0)
				return -1;
			while (n > 0 && sopath[n - 1] != '/') {
				n--;
				sopath[n] = 0;
			}
			if (n > 0 && sopath[n - 1] != '/')
				strcat(sopath, "/");
			spath = sopath;
#endif
			return 0;
		}

		/**
		 * @brief 主循环,不含windows Service
		 * @return 0
		*/
		int RunLoop()
		{
#ifndef _WIN32
			setsid(); // become session leader,不再接收父进程退出时系统发的SIGHUP信号
			CloseIO();
#endif
			std::string order;
			while (-1 == _sigval) {
				RunTime();
#ifdef _WIN32
				if (ReadMessage(order, _CTRLMAPBUF_ORDERINPOS) > 0 && !strcmp("order_stop", order.c_str())) {
					_sigval = CTRL_CLOSE_EVENT;
					break;
				}
#endif
			}
			OnStop(_sigval);
#ifndef _WIN32
			OpenMessageQueue(); //linux由子进程打开消息队列
#endif
			WirteMessage("stopped_success", _CTRLMAPBUF_ORDEROUTPOS);
			return 0;
		}

		/**
		 * @brief 子进程开始运行
		 * @param argc
		 * @param argv
		 * @return -1: failed; 0:成功； 1: 已经存在
		*/
		int StartRun(int argc, const char** argv, bool directrun = false)
		{
#ifdef _WIN32
			if (isService()) 
				return -1;
			if (!CheckPrivilege())
				return -1;
			if (CreateMessageQueue() != 0)
				return -1;
			SetConsoleCtrlHandler(exit_HandlerRoutine, TRUE);
#else
			int npid = CheckLock();
			if (npid < 0) {
				return -1;
			}
			else if (npid > 0) {
				return 1;
			}
			signal(SIGPIPE, SIG_IGN);
			signal(SIGTERM, exit_handler);
			signal(SIGINT, exit_handler);
#endif
			int nst = OnStart(argc, argv);
#ifdef _WIN32
			FreeConsole(); //释放控制台,避免控制台关闭后进程杀掉。
#else
			if (!directrun)
				OpenMessageQueue(); //linux由子进程打开消息队列
			else
				printf("%s run direct running, PID=%d, ctrl+c to exit!\n", _instname.c_str(), getpid());
#endif			
			if (nst < 0) {
				if (!directrun && WirteMessage("start_failed", _CTRLMAPBUF_ORDEROUTPOS) < 0) {
					printf("write order 'start_failed' failed!\n");
				}
				return -1;
			}
			else {
				if (!directrun && WirteMessage("start_success", _CTRLMAPBUF_ORDEROUTPOS) < 0) {
					printf("write order 'start_success' failed!\n");
				}
			}
			RunLoop();
			return 0;
		}

		/**
		 * @brief 调试模式运行, 纯应用层功能, 不附加任何功能, 可用于VC环境调试
		 * @param argc 参数个数
		 * @param argv 参数数组
		 * @return -1：失败；0:成功
		*/
		int DebugRun(int argc, const char** argv)
		{
			int nst = OnStart(argc, argv);
			if (nst < 0) {
				printf("start failed!\n");
				return -1;
			}
			else {
				printf("start success!\n");
			}
#ifdef _WIN32
			SetConsoleCtrlHandler(exit_HandlerRoutine, TRUE);
			printf("ctrl+break to exit!\n");
#else
			signal(SIGTERM, exit_handler);
			signal(SIGINT, exit_handler);
			printf("ctrl+c to exit!\n");
#endif
			while (-1 == _sigval) {
				RunTime();
			}
			OnStop(_sigval);
			return 0;
		}

	public:
		static int64_t mstime() // Return milliseconds since 1970-1-1
		{
#ifdef _WIN32
			FILETIME ft;
			GetSystemTimeAsFileTime(&ft);
			ULARGE_INTEGER ul;
			ul.LowPart = ft.dwLowDateTime;
			ul.HighPart = ft.dwHighDateTime;
			return (int64_t)((ul.QuadPart / 10000000LL) - 11644473600LL) * 1000 + (ul.QuadPart % 10000000LL) / 10000;
#else
			struct timeval tv;
			gettimeofday(&tv, NULL);
			return (int64_t)(tv.tv_sec * 1000LL + tv.tv_usec / 1000);
#endif
		}

		static void msleep(int nMilliseconds) //毫秒延迟
		{
#ifdef _WIN32
			Sleep(nMilliseconds);
#else
			usleep((nMilliseconds * 1000));
#endif
		}

		/**
		 * @brief 初始化
		 * @param instname 实例名，utf8编码, 唯一
		 * @param workpath 工作目录, utf8编码, 默认为当前执行文件目录
		 * @param pidpath pid文件目录, utf8编码, 默认值 "/var/tmp/"
		 * @return 0:成功；-1：失败
		*/
		int Init(const char* instname, const char* workpath = NULL, const char* pidpath = NULL)
		{
			if (!instname || !*instname)
				return -1;
			_instname = instname;
			if (!workpath || !*workpath) {
				if (getAppPath(_workapth) < 0)
					return -1;
			}
			else {
				_workapth = workpath;
			}
			formatPath(_workapth);

			if (!pidpath || !*pidpath) {
				_pidpathfile = "/var/tmp/";
			}
			else {
				_pidpathfile = pidpath;
			}
			formatPath(_pidpathfile);
			_pidpathfile.append(instname).append(".pid");
#ifdef _WIN32
			strcpy(_serviceName, _instname.c_str());
#endif
			return 0;
		}

		/**
		 * @brief 设置PID目录
		 * @param pidpath 目录，一般为 "/var/tmp/"或者"/var/run/"
		*/
		void setPidPath(const char* pidpath)
		{
			_pidpathfile = pidpath;
			formatPath(_pidpathfile);
			_pidpathfile.append(_instname.c_str()).append(".pid");
		}

		/**
		 * @brief 停止运行，用于信号处理函数里终止运行，不要直接调用
		 * @param sigval 信号
		*/
		inline void StopRun(int sigval)
		{
			_sigval = sigval;
		}

		/**
		 * @brief 启动,检查是否已经在运行，没有则创建一个子进程在后台运行，自己退出。
		 * @param sapp 应用名, UTF8编码
		 * @param argc 参数个数
		 * @param argv 参数数组 UTF8编码
		 * @return 0:success; -1:failed; >0成功拉起的PID
		*/
		int Start(int argc, const char** argv)
		{
			if (argc < 2) { //windows/Linux改成一致,至少带一个参数
				Usage();
				return -1;
			}
			int i = 0, npid;
			if (_instname.empty()) {
				printf("instname is empty!\n");
				return -1;
			}
			std::vector<const char*> vargs;//清零的纯应用层参数。
			vargs.reserve(argc);
			if (!strcmp(argv[1], "-start") || !strcmp(argv[1], "-run") || !strcmp(argv[1], "-service") || !strcmp(argv[1], "-debug")) {
				for (i = 0; i < argc; i++) {
					if (i != 1)
						vargs.push_back(argv[i]); //去掉"-run"，"-service", "-debug" 还原app原始命令行参数
				}
			}
			if (!strcmp(argv[1], "-debug")) { //debug不附加任何功能，纯应用层
				return DebugRun((int)vargs.size(), vargs.data());
			}
#ifdef _WIN32
			if (isService())
				return -1;
			if (!CheckPrivilege())
				return -1;
#endif
			npid = getProcessID();
			if (npid > 0) {
				printf("%s alreay runing. PID = %d\n", _instname.c_str(), npid);
				return 0;
			}
			else if (npid < 0) {
				printf("get ProcessID failed. please make sure you have administrator power.\n");
				return -1;
			}
			if (!strcmp(argv[1], "-run") || !strcmp(argv[1], "-service")) { //直接运行或者子进程运行
				return StartRun((int)vargs.size(), vargs.data(), !strcmp(argv[1], "-run"));
			}

			if (strcmp(argv[1], "-start")){
				Usage();
				return -1;
			}
			//下面检查是否运行，创建子进程在后台运行。
#ifdef _WIN32
			std::string cmdline = argv[0];
			cmdline.append(" -service"); //子进程普通模式运行
			for (i = 2; i < argc; i++) {
				cmdline.push_back(' ');
				cmdline.append(argv[i]);
			}
			STARTUPINFOW si;
			PROCESS_INFORMATION pi;

			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			ZeroMemory(&pi, sizeof(pi));

			wchar_t wcmd[512], wpath[512];
			if (!MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, wcmd, sizeof(wcmd) / sizeof(wchar_t))
				|| !MultiByteToWideChar(CP_UTF8, 0, _workapth.c_str(), -1, wpath, sizeof(wpath) / sizeof(wchar_t))) {
				return -1;
			}

			if (!CreateProcessW(NULL,   // No module name (use command line)
				wcmd,	// Command line
				NULL,	// Process handle not inheritable
				NULL,	// Thread handle not inheritable
				FALSE,	// Set handle inheritance to FALSE
				CREATE_NEW_PROCESS_GROUP,// 并使其成为新进程组进程. CREATE_NO_WINDOW会无法输出启动过程中的信息，启动成功后使用FreeConsole来脱离控制台
				NULL,	// Use parent's environment block
				wpath,	// 使用新的工作目录
				&si,// Pointer to STARTUPINFO structure
				&pi	// Pointer to PROCESS_INFORMATION structure
			)) {
				return -1;
				printf("Start failed!\n");
			}
			npid = (int)pi.dwProcessId;
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
#else		
			vargs.clear();
			vargs.push_back(argv[0]);
			vargs.push_back("-service"); // linux使用-service作为后台服务和直接启动
			for (i=2; i < argc; i++) {
				vargs.push_back(argv[i]);
			}
			vargs.push_back(NULL); //添加一个空参数结束
			if (CreateMessageQueue() < 0) { // linux由主进程或管理进程创建临时消息队列
				return -1;
			}
			npid = fork();
			if (npid == 0) { // 新建立的子进程
				//setsid(); // become session leader,不再接收父进程退出时系统发的SIGHUP信号
				//CloseIO(); 
				//上面两行移动到OnStart后，RunLoop开头，以便应用初始化能使用父进程的控制台输出错误。
				if (chdir(_workapth.c_str()) < 0) {
					printf("chdir to %s failed.\n", _workapth.c_str());
				}
				/* 实验证明父进程终止时,子进程始终会收到一个 SIGTERM 信号，使用pctrl会重复。
				prctl(PR_SET_PDEATHSIG, SIGTERM); //父进程退出时发送给子进程的信号, 强制退出用SIGKILL,
				*/
				if (execvp(vargs[0], (char**)vargs.data()) < 0) {
					printf("Start failed!\n");
					return -1;
				}
				return 0;
			}
			else if (npid < 0) {
				printf("fork failed.\n");
				return -1;
			}
#endif
			std::string order;
			int timeout = 10 * 1000, nst = 0;
			while (timeout > 0) {
#ifdef _WIN32
				nst = OpenMessageQueue();
#endif
				if (0 == nst) {
					if (ReadMessage(order, _CTRLMAPBUF_ORDEROUTPOS) > 0) {
						if (!strcmp("start_success", order.c_str())) {
							printf("start %s success PID=%d\n", _instname.c_str(), npid);
							break;
						}
						else if (!strcmp("start_failed", order.c_str())) {
							printf("start %s failed!\n", _instname.c_str());
							break;
						}
					}
				}
				msleep(100);
				timeout -= 100;
			}
#ifndef _WIN32
			if (-1 != _msgqueueid) {
				msgctl(_msgqueueid, IPC_RMID, NULL);//主进程退出时需要显示的删除消息队列，否则还在系统中。
				_msgqueueid = -1;
			}
#endif
			return npid;
		}

		/**
		 * @brief 运行状态
		 * @return 0:没有运行; >0 进程的PID;  -1：错误
		*/
		int Status()
		{
#ifdef _WIN32
			if (isService())
				return -1;
#endif
			int npid = getProcessID();
			if (npid > 0) {
				printf("%s is running! pid = %d\n", _instname.c_str(), npid);
				return npid;
			}
			else if (npid < 0) {
				printf("GetProcessID %s failed. please make sure you have administrator power.\n", _instname.c_str());
				return -1;
			}
			printf("%s is not run!\n", _instname.c_str());
			return npid;
		}

		/**
		 * @brief 终止运行
		 * @param sigkill 0：优雅退出；非0 强制退出
		 * @return -1：错误； 0:没有运行；>0终止的PID
		*/
		int Stop(int sigkill = 0)
		{
#ifdef _WIN32
			if (isService())
				return -1;
			if (!CheckPrivilege())
				return -1;
#endif
			int  npid = getProcessID();
			if (!npid) {
				printf("%s is not run.\n", _instname.c_str());
				return 0;
			}
			else if (npid < 0) {
				printf("%s getProcessID failed. please make sure you have administrator power\n", _instname.c_str());
				return -1;
			}
			if (sigkill) { //kill模式
				KillPID(npid);
				return npid;
			}
			//下面为优雅模式
#ifdef _WIN32
			if (OpenMessageQueue() < 0) {
				printf("OpenMessageQueue failed, please use -kill.\n");
				return -1;
			}
			else {
				if (WirteMessage("order_stop", _CTRLMAPBUF_ORDERINPOS) < 0) {
					printf("WirteOrder 'order_stop' to PID %d failed. please use -kill\n", npid);
					return -1;
				}
			}
#else
			if (CreateMessageQueue() < 0) { // linux由主进程或管理进程创建临时消息队列,用于接收成功停止消息
				return -1;
			}
			kill(npid, 15);
#endif			
			//检查正常退出消息
			std::string smsg;
			int timeout = 15 * 1000, nst; //15超时
			while (timeout > 0) {
				nst = ReadMessage(smsg, _CTRLMAPBUF_ORDEROUTPOS);
				if (nst > 0) {
					if (!strcmp("stopped_success", smsg.c_str())) {
						printf("stopped %s success PID=%d\n", _instname.c_str(), npid);
						break;
					}
				}
				else if (nst < 0)
					break;
				msleep(100);
				timeout -= 100;
			}
#ifndef _WIN32
			if (-1 != _msgqueueid) {
				msgctl(_msgqueueid, IPC_RMID, NULL);//主进程退出时需要显示的删除消息队列，否则还在系统中。
				_msgqueueid = -1;
			}
#endif
			return npid;
		}
	};
}// namespace ec
