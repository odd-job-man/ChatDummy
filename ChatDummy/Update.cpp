#include <Winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <queue>
#include <unordered_map>
#include "Update.h"
#include "LoginDummy.h"
#include "ChatDummy.h"
#include "Player.h"
#include "RecvJob.h"
#include "MemoryPool.h"
#include "CommonProtocol.h"
#include "Lyrics.h"

#pragma comment(lib,"MemoryPool.lib")

extern LoginDummy* g_pLoginDummy;
extern ChatDummy* g_pChatDummy;
extern CLockFreeQueue<RecvJob*> g_msgQ;
extern CTlsObjectPool<RecvJob, true> g_jobPool;

int g_needToTryConnect;
int g_loginDownClient;
int g_loginConnectFail;
int g_chatDownClient;
int g_chatConnectFail;

int g_randConnect;
int g_randDisconnect;
int g_randContents;
int g_delayAction;
int g_delayLogin;

MEMORYPOOL g_playerPool;

std::unordered_map<ULONGLONG, Player*> g_loginPlayerMap;
std::queue<Player*> g_moveQ;
std::unordered_map<ULONGLONG, Player*> g_chatPlayerMap;

BOOL g_bShutDown = FALSE;
int g_oldFrameTick;
int g_fpsCheck;
int g_time;
int g_fps;
int g_first;
int g_timeForSecond;

int g_ContentsRand;

DWORD g_rttMax;
DWORD g_rttNum;
ULONGLONG g_rttTotal;
float g_rttAvg;


__forceinline void Monitor()
{
	printf("fps : %d\n", g_fps);
}

// ����� 1�ʿ� �ѹ�����
__forceinline void CaculateRTT(DWORD curRecvTime, DWORD lastSendTime)
{
	DWORD RTT = curRecvTime - lastSendTime;
	g_rttMax = max(g_rttMax, RTT);
	++g_rttNum;
	g_rttTotal += RTT;
}
void RecvLoginJob(RecvJob* pJob);
void RecvChatJob(RecvJob* pJob);
void RecvChatMessage(Packet* pPacket, ULONGLONG sessionID);
void SendRandomChatMessage(Player* pPlayer);

void UpdateLoop(int tickPerFrame, int maxSession, int randConnect, int randDisconnect, int randContents, int delayAction, int delayLogin, const WCHAR* pIDConfigFile, const WCHAR* pLyricsConfigFile)
{
	AccountInfo::Init(pIDConfigFile);
	LyRics::Init(pLyricsConfigFile);

	g_playerPool = CreateMemoryPool(sizeof(Player), Player::MAX_PLAYER_NUM);
	g_needToTryConnect = maxSession;
	g_randConnect = randConnect;
	g_randDisconnect = randDisconnect;
	g_randContents = randContents;
	g_delayAction = delayAction;
	g_delayLogin = delayLogin;

	g_first = timeGetTime();
	g_timeForSecond = g_first;
	g_oldFrameTick = g_first;
	g_time = g_oldFrameTick;
	g_fps = 0;
	g_fpsCheck = g_oldFrameTick;

	while (!g_bShutDown)
	{
		Update();
		g_time = timeGetTime();

		if (g_time - g_timeForSecond >= 1000)
		{
			Monitor();
			g_timeForSecond += 1000;
			g_fps = 0;
		}

		//if (g_time - g_oldFrameTick >= tickPerFrame)
		//{
		//	g_oldFrameTick = g_time - ((g_time - g_first) % tickPerFrame);
		//	continue;
		//}

		//Sleep(tickPerFrame - (g_time - g_oldFrameTick));
		//g_oldFrameTick += tickPerFrame;
		//Sleep(0);
		++g_fps;
	}
}

void Update()
{
	while (1)
	{
		auto opt = g_msgQ.Dequeue();
		if (!opt.has_value())
			break;

		RecvJob* pJob = opt.value();
		switch (pJob->serverType_)
		{
		case SERVERTYPE::LOGIN:
			RecvLoginJob(pJob);
			break;
		case SERVERTYPE::CHAT:
			RecvChatJob(pJob);
			break;
		default:
			__debugbreak();
			break;
		}

		if (pJob->pPacket_ && pJob->pPacket_->DecrementRefCnt() == 0)
			PACKET_FREE(pJob->pPacket_);

		g_jobPool.Free(pJob);
	}

	// ���̿� ������ Ȯ����� Connect �õ�
	int temp = g_needToTryConnect;
	for (int i = 0; i < temp; ++i)
	{
		if (rand() % 100 + 1 <= g_randConnect)
		{
			g_pLoginDummy->Connect(false, &g_pLoginDummy->sockAddr_);
			--g_needToTryConnect;
		}
	}

	for (auto pair : g_loginPlayerMap)
	{
		Player* pPlayer = pair.second;
		if (pPlayer->bDisconnectCalled_)
			continue;

		ULONGLONG curTime = timeGetTime();
		switch (pPlayer->state_)
		{
		case STATE::LOGIN_CONNECT_SUCCESS:
		{
			if (curTime < pPlayer->lastRecvTime_ + g_delayLogin)
				break;

			// delay login ��ŭ�� �ð��� �帣�� �α��� ��Ŷ ������
			SmartPacket sp = PACKET_ALLOC(Net);
			*sp << (WORD)en_PACKET_CS_LOGIN_REQ_LOGIN << pPlayer->pInfo_->accountNo;
			sp->PutData((char*)pPlayer->pInfo_->SessionKey, Player::SESSION_KEY_LEN);
			g_pLoginDummy->SendPacket(pPlayer->sessionId_, sp);
			pPlayer->lastSendTime_ = timeGetTime();
			pPlayer->state_ = STATE::CS_LOGIN_REQ_LOGIN_SENT;
			break;
		}

		case STATE::CS_LOGIN_REQ_LOGIN_SENT:
		case STATE::LOGIN_TRY_DISCONNECT:
			break;

		default:
			__debugbreak();
			break;
		}

	}

	for (auto pair : g_chatPlayerMap)
	{
		Player* pPlayer = pair.second;
		if (pPlayer->bDisconnectCalled_)
			continue;

		ULONGLONG curTime = timeGetTime();
		switch (pPlayer->state_)
		{
		case STATE::CHAT_CONNECT_TRY:
			break;

		case STATE::CHAT_CONNECT_SUCCESS:
		{
			if (curTime < pPlayer->lastRecvTime_ + g_delayLogin)
				break;

			// delay login ��ŭ�� �ð��� �帣�� �α��� ��Ŷ ������
			SmartPacket sp = PACKET_ALLOC(Net);
			*sp << (WORD)en_PACKET_CS_CHAT_REQ_LOGIN << pPlayer->pInfo_->accountNo;
			sp->PutData((char*)pPlayer->pInfo_->ID, sizeof(WCHAR) * Player::ID_LEN);
			sp->PutData((char*)pPlayer->pInfo_->Nick, sizeof(WCHAR) * Player::SESSION_KEY_LEN);
			sp->PutData((char*)pPlayer->pInfo_->SessionKey, Player::SESSION_KEY_LEN);
			g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
			pPlayer->lastSendTime_ = timeGetTime();
			pPlayer->state_ = STATE::CS_CHAT_REQ_LOGIN_SENT;
			break;
		}

		case STATE::CS_CHAT_RES_LOGIN_RECV:
		case STATE::CS_CHAT_RES_SECTOR_MOVE_RECV:
		case STATE::CS_CHAT_RES_MESSAGE_RECV:
		{
			// Ȯ�� ����� �޽����� ������� ������ �÷��̾ ���ؼ� DELAY_ACTION��ŭ�� �ð��� �귵���� üũ�� �귵���� �޽��� ����
			if (pPlayer->bWillSendmessage_)
			{
				if (curTime < pPlayer->lastRecvTime_ + g_delayAction)
					break;

				// ���� �޽��� ����� ������
				SendRandomChatMessage(pPlayer);
				pPlayer->lastSendTime_ = timeGetTime();
				pPlayer->bWillSendmessage_ = false;
				break;
			}

			// RAND_CONNTENTS�� ��÷�̴��� Ȯ������� ��÷�̶�� �޽��� ������ ����, DEALY_ACTION��ŭ�� �ð��� �帣�� �޽��� ����
			if (rand() % 100 + 1 <= g_randContents)
			{
				pPlayer->bWillSendmessage_ = true;
			}
			break;
		}

		// �޽����� ���� ���¿����� �����̶� ��ǻ� �Ҽ��ִ°� ����.
		case STATE::CS_CHAT_REQ_LOGIN_SENT:
		case STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT:
		case STATE::CS_CHAT_REQ_MESSAGE_SENT:
		case STATE::CS_CHAT_REQ_HEARTBEAT_SENT:
			break;

		default:
			__debugbreak();
			break;
		}

		// ä�ü��� ���� ������ 30�ʿ� �ѹ� ��Ʈ��Ʈ ������
		if (curTime >= pPlayer->lastHeartbeatSendtime_ + 3000 * 10)
		{
			SmartPacket sp = PACKET_ALLOC(Net);
			*sp << (WORD)en_PACKET_CS_CHAT_REQ_HEARTBEAT;
			g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
			pPlayer->lastHeartbeatSendtime_ = timeGetTime();
		}
	}
}

void RecvLoginJob(RecvJob* pJob)
{
	switch (pJob->type_)
	{
	case JOBTYPE::CONNECT_SUCCESS:
	{
		Player* pPlayer = (Player*)AllocMemoryFromPool(g_playerPool);
		pPlayer->serverType_ = SERVERTYPE::LOGIN;
		pPlayer->sessionId_ = pJob->sessionId_;
		pPlayer->lastRecvTime_ = timeGetTime();
		pPlayer->state_ = STATE::LOGIN_CONNECT_SUCCESS;
		pPlayer->bDisconnectCalled_ = false;
		pPlayer->pInfo_ = AccountInfo::Alloc();
		pPlayer->bAuthAtLoginServer_ = false;
		pPlayer->bWillSendmessage_ = false;
		g_loginPlayerMap.insert(std::make_pair(pJob->sessionId_, pPlayer));
		break;
	}
	case JOBTYPE::RECV_MESSAGE:
	{
		Packet* pPacket = pJob->pPacket_;

		Player* pPlayer = g_loginPlayerMap.find(pJob->sessionId_)->second;
		pPlayer->lastRecvTime_ = timeGetTime();
		CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);

		WORD type;
		BYTE status;
		INT64 accountNo;
		*pPacket >> type;

		if (type != en_PACKET_CS_LOGIN_RES_LOGIN)
			__debugbreak();

		*pPacket >> accountNo >> status;

		if (status != 1)
			__debugbreak();

		if (pPlayer->pInfo_->accountNo != accountNo)
			__debugbreak();

		WCHAR* pID = (WCHAR*)pPacket->GetPointer(sizeof(WCHAR) * Player::ID_LEN);
		WCHAR* pNick = (WCHAR*)pPacket->GetPointer(sizeof(WCHAR) * Player::NICK_NAME_LEN);

		USHORT gamePort;
		USHORT chatPort;

		WCHAR* pGameIP = (WCHAR*)pPacket->GetPointer(sizeof(WCHAR) * 16);
		*pPacket >> gamePort;

		WCHAR* pChatIP = (WCHAR*)pPacket->GetPointer(sizeof(WCHAR) * 16);
		*pPacket >> chatPort;

		// ä�ü��� Connect�� SOCKADDR_IN �����
		ZeroMemory(&pPlayer->sockAddr_, sizeof(SOCKADDR_IN));
		pPlayer->sockAddr_.sin_family = AF_INET;
		InetPtonW(AF_INET, pChatIP, &pPlayer->sockAddr_.sin_addr);
		pPlayer->sockAddr_.sin_port = htons(chatPort);

		pPlayer->state_ = STATE::LOGIN_TRY_DISCONNECT;
		pPlayer->bDisconnectCalled_ = true;
		g_pLoginDummy->Disconnect(pJob->sessionId_);
		break;
	}
	case JOBTYPE::ON_RELEASE:
	{
		auto iter = g_loginPlayerMap.find(pJob->sessionId_);
		Player* pPlayer = iter->second;

		if (pPlayer->bDisconnectCalled_ && pPlayer->state_ == STATE::LOGIN_TRY_DISCONNECT) // �����޽����� �����ؼ� Ŭ�󿡼� ���������� Disconnect �� ���
		{
			pPlayer->state_ = STATE::CHAT_CONNECT_TRY;
			g_moveQ.push(pPlayer);

			g_loginPlayerMap.erase(iter);
			g_pChatDummy->Connect(false, &pPlayer->sockAddr_);
			break;
		}

		// �α��� ���� ���� �޽����� �������Լ� ���� ���޴µ� ������ ������ 
		++g_loginDownClient;
		break;
	}
	case JOBTYPE::CONNECT_FAIL:
		++g_loginConnectFail;
		break;
	case JOBTYPE::ALL_RECONNECT_FAIL:
		++g_needToTryConnect;
		break;
	default:
		__debugbreak();
		break;
	}
}

void RecvChatJob(RecvJob* pJob)
{
	switch (pJob->type_)
	{
	case JOBTYPE::CONNECT_SUCCESS:
	{
		Player* pPlayer = g_moveQ.front();
		g_moveQ.pop();
		pPlayer->serverType_ = SERVERTYPE::CHAT;
		pPlayer->sessionId_ = pJob->sessionId_;
		pPlayer->lastRecvTime_ = timeGetTime();
		pPlayer->lastHeartbeatSendtime_ = timeGetTime();
		pPlayer->state_ = STATE::CHAT_CONNECT_SUCCESS;
		pPlayer->bDisconnectCalled_ = false;
		pPlayer->bAuthAtChatServer_ = false;
		pPlayer->bRegisterAtSector_ = false;
		pPlayer->bWillSendmessage_ = false;
		g_chatPlayerMap.insert(std::make_pair(pJob->sessionId_, pPlayer));
		break;
	}
	case JOBTYPE::RECV_MESSAGE:
	{
		RecvChatMessage(pJob->pPacket_, pJob->sessionId_);
		break;
	}
	case JOBTYPE::ON_RELEASE:
	{
		auto iter = g_chatPlayerMap.find(pJob->sessionId_);
		Player* pPlayer = iter->second;

		if (!pPlayer->bDisconnectCalled_ || pPlayer->state_ != STATE::CHAT_TRY_DISCONNECT) // �����޽����� �����ؼ� Ŭ�󿡼� ���������� Disconnect �� ���
			++g_chatDownClient;

		g_chatPlayerMap.erase(iter);
		++g_needToTryConnect;
		break;
	}
	case JOBTYPE::CONNECT_FAIL:
		++g_chatConnectFail;
		break;

	case JOBTYPE::ALL_RECONNECT_FAIL:
		g_moveQ.pop();
		++g_needToTryConnect;
		break;
	default:
		__debugbreak();
		break;
	}
}

void RecvChatMessage(Packet* pPacket, ULONGLONG sessionID)
{
	Player* pPlayer = g_chatPlayerMap.find(sessionID)->second;
	pPlayer->lastRecvTime_ = timeGetTime();
	CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);

	WORD type;
	*pPacket >> type;
	switch ((en_PACKET_TYPE)type)
	{
	case en_PACKET_CS_CHAT_RES_LOGIN:
	{
		if (pPlayer->state_ != STATE::CS_CHAT_REQ_LOGIN_SENT)
		{
			LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Previous Send Message Is Not CS_CHAT_REQ_LOGIN");
			__debugbreak();
		}

		BYTE status;
		INT64 accountNo;

		*pPacket >> status >> accountNo;
		if (status != 1)
			__debugbreak();

		if (accountNo != pPlayer->pInfo_->accountNo)
			__debugbreak();

		pPlayer->state_ = STATE::CS_CHAT_RES_LOGIN_RECV;
		pPlayer->bAuthAtChatServer_ = true;
		break;
	}
	case en_PACKET_CS_CHAT_RES_SECTOR_MOVE:
	{
		if (pPlayer->state_ != STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT)
		{
			LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Previous Send Message Is Not CS_CHAT_REQ_SECTOR_MOVE");
			__debugbreak();
		}

		INT64 accountNo;
		WORD sectorX, sectorY;
		*pPacket >> accountNo >> sectorX >> sectorY;

		if (pPlayer->sectorX_ != sectorX || pPlayer->sectorY_ != sectorY)
			__debugbreak();

		pPlayer->state_ = STATE::CS_CHAT_RES_SECTOR_MOVE_RECV;
		pPlayer->bRegisterAtSector_ = true;
		break;
	}

	case en_PACKET_CS_CHAT_RES_MESSAGE:
	{
		if (!pPlayer->bRegisterAtSector_)
		{
			LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Player Is Not Registered At Sector But Chat Message Recved");
			__debugbreak();
		}

		INT64	accountNo;
		*pPacket >>  accountNo;
		WCHAR* pID = (WCHAR*)pPacket->GetPointer(sizeof(WCHAR) * Player::ID_LEN);
		WCHAR* pNick = (WCHAR*)pPacket->GetPointer(sizeof(WCHAR) * Player::NICK_NAME_LEN);
		WORD messageByte;
		*pPacket >> messageByte;
		WCHAR* pChat = (WCHAR*)pPacket->GetPointer(messageByte);

		if (accountNo == pPlayer->pInfo_->accountNo) // ������ ���� �޽����� ������ �������(�������)
		{
			// ������ ���� �޽����� ������ �޾Ѵµ� ������ ���� �޽����� ä�ø޽��� ��û�� �ƴҶ�
			if (pPlayer->state_ != STATE::CS_CHAT_REQ_MESSAGE_SENT)
			{
				LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Previous Send Message Is Not CS_CHAT_REQ_MESSAGE", pPlayer->pInfo_->ID, pID);
				__debugbreak();
			}

			bool bWrong = false;
			if (0 != wcscmp(pID, pPlayer->pInfo_->ID))
			{
				LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Send ID : %s Recv ID : %s", pPlayer->pInfo_->ID, pID);
				bWrong = true;
			}

			if (0 != wcscmp(pNick, pPlayer->pInfo_->Nick))
			{
				LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Send Nick : %s Recv Nick : %s", pPlayer->pInfo_->Nick, pNick);
				bWrong = true;
			}

			if (messageByte / 2 != pPlayer->pLastSentChat->len)
			{
				LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Send Len : %d Recv Len : %d", pPlayer->pLastSentChat->len, messageByte / 2);
				bWrong = true;
			}

			if (0 != wcsncmp(pChat, pPlayer->pLastSentChat->pLyrics, messageByte / 2))
			{
				LOG(L"RECV_PACKET_ERROR", ERR, TEXTFILE, L"Send Chat : %s Recv Chat : %s", pPlayer->pLastSentChat->pLyrics, pChat);
				bWrong = true;
			}

			if (bWrong)
				__debugbreak();

			pPlayer->state_ = STATE::CS_CHAT_RES_MESSAGE_RECV;
		}
		break;
	}
	default:
		__debugbreak();
		break;
	}

	// �޽��� ���Ž� ���� Ȯ���� Disconnect
	if (rand() % 100 + 1 <= g_randDisconnect)
	{
		pPlayer->bDisconnectCalled_ = true;
		pPlayer->state_ = STATE::CHAT_TRY_DISCONNECT;
		g_pChatDummy->Disconnect(sessionID);
		return;
	}

}

void SendRandomChatMessage(Player* pPlayer)
{
	SmartPacket sp = PACKET_ALLOC(Net);
	if (!pPlayer->bRegisterAtSector_ || rand() % 2 == 0) // ���Ϳ� ����� �ȵ������� ������ �����̵� ������, �װԾƴϸ� Ȯ��������
	{
		pPlayer->sectorX_ = rand() % 49 + 1;
		pPlayer->sectorY_ = rand() % 49 + 1;
		*sp << (WORD)en_PACKET_CS_CHAT_REQ_SECTOR_MOVE << pPlayer->pInfo_->accountNo << pPlayer->sectorX_ << pPlayer->sectorY_;
		pPlayer->state_ = STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT;
	}
	else
	{
		const LyRics* pLR = pPlayer->pLastSentChat = LyRics::Get(); // ���߿� ���ο��� �����ö� �����Ϸ��� �����ص־���
		*sp << (WORD)en_PACKET_CS_CHAT_REQ_MESSAGE << pPlayer->pInfo_->accountNo;
		*sp << (WORD)(sizeof(WCHAR) * pLR->len);
		sp->PutData((char*)pLR->pLyrics, sizeof(WCHAR) * pLR->len);
		pPlayer->state_ = STATE::CS_CHAT_REQ_MESSAGE_SENT;
	}
	g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
	pPlayer->lastSendTime_ = timeGetTime();
}
