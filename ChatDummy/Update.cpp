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

// 평균은 1초에 한번구함
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

	// 더미에 설정된 확률대로 Connect 시도
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

			// delay login 만큼의 시간이 흐르면 로그인 패킷 보내기
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

			// delay login 만큼의 시간이 흐르면 로그인 패킷 보내기
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
			// 확률 계산후 메시지를 보내기로 결정된 플레이어에 대해서 DELAY_ACTION만큼의 시간이 흘럿는지 체크후 흘럿으면 메시지 보냄
			if (pPlayer->bWillSendmessage_)
			{
				if (curTime < pPlayer->lastRecvTime_ + g_delayAction)
					break;

				// 보낼 메시지 만들고 보내기
				SendRandomChatMessage(pPlayer);
				pPlayer->lastSendTime_ = timeGetTime();
				pPlayer->bWillSendmessage_ = false;
				break;
			}

			// RAND_CONNTENTS에 당첨됫는지 확률계산후 당첨이라면 메시지 보내기 예약, DEALY_ACTION만큼의 시간이 흐르면 메시지 전송
			if (rand() % 100 + 1 <= g_randContents)
			{
				pPlayer->bWillSendmessage_ = true;
			}
			break;
		}

		// 메시지를 보낸 상태에서는 핑퐁이라서 사실상 할수있는게 없다.
		case STATE::CS_CHAT_REQ_LOGIN_SENT:
		case STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT:
		case STATE::CS_CHAT_REQ_MESSAGE_SENT:
		case STATE::CS_CHAT_REQ_HEARTBEAT_SENT:
			break;

		default:
			__debugbreak();
			break;
		}

		// 채팅서버 접속 유저는 30초에 한번 하트비트 보내기
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

		// 채팅서버 Connect할 SOCKADDR_IN 만들기
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

		if (pPlayer->bDisconnectCalled_ && pPlayer->state_ == STATE::LOGIN_TRY_DISCONNECT) // 성공메시지를 수신해서 클라에서 정상적으로 Disconnect 한 경우
		{
			pPlayer->state_ = STATE::CHAT_CONNECT_TRY;
			g_moveQ.push(pPlayer);

			g_loginPlayerMap.erase(iter);
			g_pChatDummy->Connect(false, &pPlayer->sockAddr_);
			break;
		}

		// 로그인 인증 성공 메시지를 서버에게서 받지 못햇는데 연결이 끊긴경우 
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

		if (!pPlayer->bDisconnectCalled_ || pPlayer->state_ != STATE::CHAT_TRY_DISCONNECT) // 성공메시지를 수신해서 클라에서 정상적으로 Disconnect 한 경우
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

		if (accountNo == pPlayer->pInfo_->accountNo) // 본인이 보낸 메시지를 본인이 받은경우(검증대상)
		{
			// 본인이 보낸 메시지를 본인이 받앗는데 이전에 보낸 메시지가 채팅메시지 요청이 아닐때
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

	// 메시지 수신시 일정 확률로 Disconnect
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
	if (!pPlayer->bRegisterAtSector_ || rand() % 2 == 0) // 섹터에 등록이 안되잇으면 무조건 섹터이동 보내기, 그게아니면 확률적결정
	{
		pPlayer->sectorX_ = rand() % 49 + 1;
		pPlayer->sectorY_ = rand() % 49 + 1;
		*sp << (WORD)en_PACKET_CS_CHAT_REQ_SECTOR_MOVE << pPlayer->pInfo_->accountNo << pPlayer->sectorX_ << pPlayer->sectorY_;
		pPlayer->state_ = STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT;
	}
	else
	{
		const LyRics* pLR = pPlayer->pLastSentChat = LyRics::Get(); // 나중에 본인에게 핑퐁올때 검증하려면 저장해둬야함
		*sp << (WORD)en_PACKET_CS_CHAT_REQ_MESSAGE << pPlayer->pInfo_->accountNo;
		*sp << (WORD)(sizeof(WCHAR) * pLR->len);
		sp->PutData((char*)pLR->pLyrics, sizeof(WCHAR) * pLR->len);
		pPlayer->state_ = STATE::CS_CHAT_REQ_MESSAGE_SENT;
	}
	g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
	pPlayer->lastSendTime_ = timeGetTime();
}
