#include "ChatDummy.h"
#include "RecvJob.h"
#include "Player.h"

extern CLockFreeQueue<RecvJob*> g_msgQ;
extern CTlsObjectPool<RecvJob, true> g_jobPool;

ChatDummy::ChatDummy(const BOOL bAutoReconnect, const LONG autoReconnectCnt, const LONG autoReconInterval, const WCHAR* pConfigFile)
	:NetClient{ bAutoReconnect,autoReconnectCnt,autoReconInterval,pConfigFile }, pConfigFile_{ pConfigFile }
{
}

BOOL ChatDummy::Start()
{
	for (DWORD i = 0; i < IOCP_WORKER_THREAD_NUM_; ++i)
		ResumeThread(hIOCPWorkerThreadArr_[i]);

	return TRUE;
}

void ChatDummy::OnRecv(ULONGLONG id, SmartPacket& sp)
{
	int a;
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::CHAT, JOBTYPE::RECV_MESSAGE, id, sp.GetPacket()));
}

void ChatDummy::OnError(ULONGLONG id, int errorType, Packet* pRcvdPacket)
{
}

void ChatDummy::OnConnect(ULONGLONG id)
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::CHAT, JOBTYPE::CONNECT_SUCCESS, id));
}

void ChatDummy::OnRelease(ULONGLONG id)
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::CHAT, JOBTYPE::ON_RELEASE, id));
}

void ChatDummy::OnConnectFailed(ULONGLONG id)
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::CHAT, JOBTYPE::CONNECT_FAIL, id));
}

void ChatDummy::OnAutoResetAllFailed()
{
	g_msgQ.Enqueue(g_jobPool.Alloc(SERVERTYPE::CHAT, JOBTYPE::ALL_RECONNECT_FAIL, MAXULONGLONG));
}
