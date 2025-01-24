#include "LoginDummy.h"
#include "Player.h"
#include "CommonProtocol.h"
#include "AccountInfo.h"
#include <vector>
#include "CLockFreeQueue.h"
#include "RecvJob.h"

CLockFreeQueue<RecvJob*> g_msgQ;
CTlsObjectPool<RecvJob, true> g_jobPool;

LoginDummy::LoginDummy(BOOL bAutoReconnect, LONG autoReconnectCnt, LONG autoReconnectInterval, BOOL bUseMemberSockAddrIn, WCHAR* pIP, USHORT port, DWORD iocpWorkerThreadNum, DWORD cuncurrentThreadNum, LONG maxSession, BYTE packetCode, BYTE packetFixedKey)
	:NetClient{ bAutoReconnect,autoReconnectCnt,autoReconnectInterval,bUseMemberSockAddrIn,pIP,port,iocpWorkerThreadNum,cuncurrentThreadNum,maxSession,packetCode,packetFixedKey }
{
}

//LoginDummy::LoginDummy(const BOOL bAutoReconnect, const LONG autoReconnectCnt, const LONG autoReconnectInterval, const WCHAR* pConfigFile)
//	:NetClient{ bAutoReconnect,autoReconnectCnt,autoReconnectInterval,pConfigFile }, pConfigFile_{ pConfigFile }
//{
//}

BOOL LoginDummy::Start()
{
	for (DWORD i = 0; i < IOCP_WORKER_THREAD_NUM_; ++i)
		ResumeThread(hIOCPWorkerThreadArr_[i]);
	return TRUE;
}

LoginDummy::~LoginDummy()
{
}

void LoginDummy::OnRecv(ULONGLONG id, SmartPacket& sp)
{
	sp->IncreaseRefCnt();
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::LOGIN, JOBTYPE::RECV_MESSAGE, id, sp.GetPacket()));
}

void LoginDummy::OnError(ULONGLONG id, int errorType, Packet* pRcvdPacket)
{
}

void LoginDummy::OnConnect(ULONGLONG id)
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::LOGIN, JOBTYPE::CONNECT_SUCCESS, id));
}

void LoginDummy::OnRelease(ULONGLONG id)
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::LOGIN, JOBTYPE::ON_RELEASE, id));
}

void LoginDummy::OnConnectFailed(ULONGLONG id)
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::LOGIN, JOBTYPE::CONNECT_FAIL, id));
}

void LoginDummy::OnAutoResetAllFailed()
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::LOGIN, JOBTYPE::ALL_RECONNECT_FAIL, MAXULONGLONG));
}
