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
#include "Monitor.h"
#include "MakePacket.h"
#pragma comment(lib,"MemoryPool.lib")

extern LoginDummy* g_pLoginDummy;
extern ChatDummy* g_pChatDummy;
extern CLockFreeQueue<RecvJob*> g_msgQ;
extern CTlsObjectPool<RecvJob, true> g_jobPool;

int g_maxSession;
int g_needToTryConnect;

int g_loginConnectTotal;
int g_loginDownClient;
int g_loginConnectFail;

int g_chatConnectTotal;
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
std::unordered_map<ULONGLONG, Player*> g_chatConnectPlayerMap;
std::unordered_map<ULONGLONG, Player*> g_chatLoginPlayerMap;
std::stack<const AccountInfo*> g_NonConnectedStack;

bool g_bShutDown;
int g_loopNum;
int g_first;
int g_timeForSecond;
int g_ContentsRand;

// ���� ChatMap�ȿ��� 
int g_ChatConnectNum;
int g_ChatLoginNum;

DWORD g_rttMax;
DWORD g_rttNum;
ULONGLONG g_rttTotal;
float g_rttAvg;

bool g_bPlay;
bool g_bConnectStop;
bool g_bDuplicateLoginOn;
bool g_bAttackOn;


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


void UpdateLoop(int maxSession, int randConnect, int randDisconnect, int randContents, int delayAction, int delayLogin, const WCHAR* pIDConfigFile, const WCHAR* pLyricsConfigFile)
{
	g_bPlay = true;
	g_bConnectStop = g_bDuplicateLoginOn = g_bAttackOn = false;

	AccountInfo::Init(pIDConfigFile);
	LyRics::Init(pLyricsConfigFile);

	for (int i = 0; i < maxSession; ++i)
	{
		g_NonConnectedStack.push(AccountInfo::Alloc());
	}

	g_playerPool = CreateMemoryPool(sizeof(Player), Player::MAX_PLAYER_NUM);
	g_maxSession = g_needToTryConnect = maxSession;
	g_randConnect = randConnect;
	g_randDisconnect = randDisconnect;
	g_randContents = randContents;
	g_delayAction = delayAction;
	g_delayLogin = delayLogin;

	g_first = timeGetTime();
	g_timeForSecond = g_first;
	g_loopNum = 0;


	while (!g_bShutDown)
	{
		Update();
		if (timeGetTime() - g_timeForSecond >= 1000)
		{
			//Monitor();
			printf("%d\n", g_loopNum);
			g_timeForSecond += 1000;
			g_loopNum = 0;

			// S : Stop
			//if (GetAsyncKeyState() & 0x0001)
			//{

			//}
			//Q : ShutDown
			if (GetAsyncKeyState(0x51) & 0x0001)
			{
				g_bShutDown = true;
			}
		}


		Sleep(3);
		++g_loopNum;
	}

	// �˴ٿ� ����
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
	if (!g_bConnectStop)
	{
		for (int i = 0; i < temp; ++i)
		{
			if (rand() % 100 + 1 <= g_randConnect)
			{
				g_pLoginDummy->Connect(false, &g_pLoginDummy->sockAddr_);
				--g_needToTryConnect;
			}
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
			MAKE_CS_LOGIN_REQ_LOGIN(pPlayer->pInfo_->accountNo, pPlayer->pInfo_->SessionKey, sp);
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

	ULONGLONG curTime = timeGetTime();
	for (auto pair : g_chatConnectPlayerMap)
	{
		Player* pPlayer = pair.second;
		if (pPlayer->state_ == STATE::CS_CHAT_REQ_LOGIN_SENT)
			continue;

		if (pPlayer->state_ != STATE::CHAT_CONNECT_SUCCESS)
			__debugbreak();

		if (curTime < pPlayer->lastRecvTime_ + g_delayLogin)
			continue;

		// delay login ��ŭ�� �ð��� �帣�� �α��� ��Ŷ ������
		SmartPacket sp = PACKET_ALLOC(Net);
		MAKE_CS_CHAT_REQ_LOGIN(pPlayer->pInfo_->accountNo, pPlayer->pInfo_->ID, pPlayer->pInfo_->Nick, pPlayer->pInfo_->SessionKey, sp);
		g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
		pPlayer->lastSendTime_ = timeGetTime();
		pPlayer->state_ = STATE::CS_CHAT_REQ_LOGIN_SENT;
	}

	curTime = timeGetTime();
	for (auto pair : g_chatLoginPlayerMap)
	{
		Player* pPlayer = pair.second;
		if (pPlayer->bDisconnectCalled_)
			continue;

		switch (pPlayer->state_)
		{
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
		case STATE::CHAT_TRY_DISCONNECT:
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
		pPlayer->pInfo_ = g_NonConnectedStack.top();
		pPlayer->bDisconnectCalled_ = false;
		pPlayer->bAuthAtLoginServer_ = false;
		pPlayer->bWillSendmessage_ = false;

		g_NonConnectedStack.pop();
		g_loginPlayerMap.insert(std::make_pair(pJob->sessionId_, pPlayer));
		++g_loginConnectTotal;
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
			g_pChatDummy->Connect(false, &pPlayer->sockAddr_);
		}
		else // �α��� ���� ���� �޽����� �������Լ� ���� ���޴µ� ������ ������
		{
			++g_loginDownClient;
			++g_needToTryConnect;
			g_NonConnectedStack.push(pPlayer->pInfo_);
			RetMemoryToPool(g_playerPool, pPlayer);
		}
		g_loginPlayerMap.erase(iter);
		break;
	}
	case JOBTYPE::CONNECT_FAIL:
		++g_loginConnectFail;
		break;
	case JOBTYPE::ALL_RECONNECT_FAIL:
	{
		++g_needToTryConnect;
		g_moveQ.pop();
		break;
	}
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
		g_chatConnectPlayerMap.insert(std::make_pair(pJob->sessionId_, pPlayer));
		++g_chatConnectTotal;
		break;
	}
	case JOBTYPE::RECV_MESSAGE:
	{
		RecvChatMessage(pJob->pPacket_, pJob->sessionId_);
		break;
	}
	case JOBTYPE::ON_RELEASE:
	{
		auto iter = g_chatLoginPlayerMap.find(pJob->sessionId_);
		if (iter == g_chatLoginPlayerMap.end())
		{
			iter = g_chatConnectPlayerMap.find(pJob->sessionId_);
		}

		Player* pPlayer = iter->second;

		if (!pPlayer->bDisconnectCalled_ || pPlayer->state_ != STATE::CHAT_TRY_DISCONNECT) // �����޽����� �����ؼ� Ŭ�󿡼� ���������� Disconnect �� ���
			++g_chatDownClient;

		++g_needToTryConnect;
		g_chatLoginPlayerMap.erase(iter);
		g_NonConnectedStack.push(pPlayer->pInfo_);
		RetMemoryToPool(g_playerPool, pPlayer);
		break;
	}
	case JOBTYPE::CONNECT_FAIL:
		++g_chatConnectFail;
		break;

	case JOBTYPE::ALL_RECONNECT_FAIL:
	{
		++g_needToTryConnect;
		Player* pPlayer = g_moveQ.front();
		g_moveQ.pop();
		g_NonConnectedStack.push(pPlayer->pInfo_);
		RetMemoryToPool(g_playerPool, pPlayer);
		break;
	}
	default:
		__debugbreak();
		break;
	}
}

void RecvChatMessage(Packet* pPacket, ULONGLONG sessionID)
{
	Player* pPlayer = nullptr; 

	WORD type;
	*pPacket >> type;
	switch ((en_PACKET_TYPE)type)
	{
	case en_PACKET_CS_CHAT_RES_LOGIN:
	{
		pPlayer = g_chatConnectPlayerMap.find(sessionID)->second;

		if (pPlayer->state_ == STATE::CHAT_TRY_DISCONNECT)
			break;

		pPlayer->lastRecvTime_ = timeGetTime();
		CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);
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
		g_chatConnectPlayerMap.erase(sessionID);
		g_chatLoginPlayerMap.insert(std::make_pair(sessionID, pPlayer));
		break;
	}
	case en_PACKET_CS_CHAT_RES_SECTOR_MOVE:
	{
		pPlayer = g_chatLoginPlayerMap.find(sessionID)->second;

		if (pPlayer->state_ == STATE::CHAT_TRY_DISCONNECT)
			break;

		pPlayer->lastRecvTime_ = timeGetTime();
		CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);
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
		break;
	}

	case en_PACKET_CS_CHAT_RES_MESSAGE:
	{
		pPlayer = g_chatLoginPlayerMap.find(sessionID)->second;

		if (pPlayer->state_ == STATE::CHAT_TRY_DISCONNECT)
			break;

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
			pPlayer->lastRecvTime_ = timeGetTime();
			CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);
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
			break;
		}
		else
		{
			return;
		}
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
	if (!pPlayer->bRegisterAtSector_ || rand() % 5 < 1) // ���Ϳ� ����� �ȵ������� ������ �����̵� ������, �װԾƴϸ� Ȯ��������
	{
		pPlayer->sectorX_ = rand() % 49 + 1;
		pPlayer->sectorY_ = rand() % 49 + 1;
		MAKE_CS_CHAT_REQ_SECTOR_MOVE(pPlayer->pInfo_->accountNo, pPlayer->sectorX_, pPlayer->sectorY_, sp);
		pPlayer->state_ = STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT;
		pPlayer->bRegisterAtSector_ = true; // ���⼭ �̸�����ؾ� �׻��̿� RES_MESSAGE �ٸ� ������ ���� �޽ð� ������ ���Ϳ��� ����� �Ǿ����� ������ ä�ø޽����� �޾Ѵٴ� ���� ������ �Ȼ���
	}
	else
	{
		const LyRics* pLR = pPlayer->pLastSentChat = LyRics::Get(); // ���߿� ���ο��� �����ö� �����Ϸ��� �����ص־���
		MAKE_CS_CHAT_REQ_MESSAGE(pPlayer->pInfo_->accountNo, pLR->len, pLR->pLyrics, sp);
		pPlayer->state_ = STATE::CS_CHAT_REQ_MESSAGE_SENT;
	}
	g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
	pPlayer->lastSendTime_ = timeGetTime();
}
