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

#define VK_S 0x53
#define VK_A 0x41 
#define VK_W 0x57
#define VK_T 0x54
#define VK_R 0x52
#define VK_Q 0x51

extern LoginDummy* g_pLoginDummy;
extern ChatDummy* g_pChatDummy;
extern CLockFreeQueue<RecvJob*> g_msgQ;
extern CTlsObjectPool<RecvJob, true> g_jobPool;

int g_maxSession;
int g_disconnectedNum;

int g_loginConnectTotal;
int g_loginDownClient;
int g_loginConnectFail;

int g_duplicateConnectFail;

int g_chatConnectTotal;
int g_chatDownClient;
int g_chatConnectFail;

constexpr int RAND_ATTACK = 10;
constexpr int RAND_DUPLICATE_LOGIN = 30;
constexpr int RAND_TIMEOUT = 10;

int g_randConnect;
int g_randDisconnect;
int g_randContents;
int g_delayAction;
int g_delayLogin;
int g_replyWait;

MEMORYPOOL g_playerPool;

std::unordered_map<ULONGLONG, Player*> g_loginPlayerMap;
std::queue<Player*> g_moveQ;
std::unordered_map<ULONGLONG, Player*> g_chatConnectPlayerMap;
std::unordered_map<ULONGLONG, Player*> g_chatLoginPlayerMap;
std::stack<const AccountInfo*> g_NonConnectedStack;

int g_loopNum;
int g_first;
DWORD g_timeForSecond;
int g_ContentsRand;
DWORD g_timeForAvgReset;

// ���� ChatMap�ȿ��� 
int g_chatConnectNum;
int g_chatLoginNum;

DWORD g_rttMin;
DWORD g_rttMax;
DWORD g_rttNum;
DWORD g_rttTotal;
float g_rttAvg;

bool g_bShutDown;
bool g_bPlay;
bool g_bConnectStop;
bool g_bDuplicateLoginOn;
bool g_bTestTimeOut;

int g_duplicateLoginTPS;
int g_sessionTimeOutTPS;
int g_loginTimeOutTPS;

DWORD g_loginTimeOutTotal;
DWORD g_loginTimeOutNum;

DWORD g_sessionTimeOutTotal;
DWORD g_sessionTimeOutNum;


// ����� 1�ʿ� �ѹ�����
__forceinline void CaculateRTT(DWORD curRecvTime, DWORD lastSendTime)
{
	DWORD RTT = curRecvTime - lastSendTime;
	g_rttMin = min(g_rttMin, RTT);
	g_rttMax = max(g_rttMax, RTT);
	++g_rttNum;
	g_rttTotal += RTT;
}

__forceinline void RefreshLoginTimoutTotal(DWORD disconnectTime, DWORD loginTime)
{
	g_loginTimeOutTotal += (disconnectTime - loginTime);
	++g_loginTimeOutNum;
}

__forceinline void RefreshSessionTimoutTotal(DWORD disconnectTime, DWORD connectTime)
{
	g_sessionTimeOutTotal += (disconnectTime - connectTime);
	++g_sessionTimeOutNum;
}

void RecvLoginJob(RecvJob* pJob);
void RecvChatJob(RecvJob* pJob);
void RecvChatMessage(Packet* pPacket, ULONGLONG sessionID);
void SendRandomChatMessage(Player* pPlayer);


void UpdateLoop(int maxSession, int randConnect, int randDisconnect, int randContents, int delayAction, int delayLogin, const WCHAR* pIDConfigFile, const WCHAR* pLyricsConfigFile)
{
	srand((unsigned)time(nullptr));
	g_bPlay = true;
	MonitorInit();
	AccountInfo::Init(pIDConfigFile);
	LyRics::Init(pLyricsConfigFile);

	for (int i = 0; i < maxSession; ++i)
	{
		g_NonConnectedStack.push(AccountInfo::Alloc());
	}

	g_playerPool = CreateMemoryPool(sizeof(Player), Player::MAX_PLAYER_NUM);
	g_maxSession = g_disconnectedNum = maxSession;
	g_randConnect = randConnect;
	g_randDisconnect = randDisconnect;
	g_randContents = randContents;
	g_delayAction = delayAction;
	g_delayLogin = delayLogin;

	g_first = timeGetTime();
	g_timeForAvgReset = g_timeForSecond = g_first;
	g_loopNum = 0;

	while (!g_bShutDown)
	{
		Update();
		DWORD curTime = timeGetTime();
		if (curTime >= g_timeForSecond + 1000)
		{
			Monitor();
			g_timeForSecond += 1000;
			g_loopNum = 0;

			if (curTime >= g_timeForAvgReset + 1000 * 60)
			{
				g_rttNum = 0;
				g_rttTotal = 0;
				g_loginTimeOutTotal = 0;
				g_loginTimeOutNum = 0;
				g_sessionTimeOutTotal = 0;
				g_sessionTimeOutNum = 0;
				g_timeForAvgReset += 1000 * 60;
			}

			if (GetAsyncKeyState(VK_S) & 0x0001)
			{
				g_bPlay = !g_bPlay;
			}

			if (GetAsyncKeyState(VK_A) & 0x0001)
			{
				g_bConnectStop = !g_bConnectStop;
			}

			// �ڵ��� ������ ���� ���߷α���, ATTACK, TIMEOUT TEST�� ������ ������ �� �� �ϳ��� Ȱ��ȭ ����
			if (GetAsyncKeyState(VK_W) & 0x0001)
			{
				g_bDuplicateLoginOn = !g_bDuplicateLoginOn;
			}

			if (GetAsyncKeyState(VK_R) & 0x0001)
			{
				g_bTestTimeOut= !g_bTestTimeOut;
			}

			if (GetAsyncKeyState(VK_Q) & 0x0001)
			{
				g_bShutDown = true;
			}
		}
		Sleep(0);
		++g_loopNum;
	}
	// �˴ٿ� ����
}

void Update()
{
	while (1)
	{
		const auto& opt = g_msgQ.Dequeue();
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

	// Stop�϶��� �����޽����� ���� �ؼ��ؼ� ReplyWait�� ���ҽ�Ű�� �������� �޽��� ������ ���Ѵ�.
	if (!g_bPlay) return;

	// ���̿� ������ Ȯ����� Connect �õ�
	int temp = g_disconnectedNum;
	if (!g_bConnectStop)
	{
		for (int i = 0; i < temp; ++i)
		{
			if (rand() % 100 < g_randConnect)
			{
				g_pLoginDummy->Connect(false, &g_pLoginDummy->sockAddr_);
				--g_disconnectedNum;
			}
		}
	}


	for (auto pair : g_loginPlayerMap)
	{
		Player* pPlayer = pair.second;
		if((pPlayer->field_ & DISCONNECT_TARGET) == DISCONNECT_TARGET)
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
			++g_replyWait;
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

		if (pPlayer->state_ != STATE::CHAT_CONNECT_SUCCESS && pPlayer->state_ != STATE::CS_CHAT_REQ_LOGIN_SENT)
			__debugbreak();

		if ((pPlayer->field_ & SESSION_TIMEOUT_TARGET) == SESSION_TIMEOUT_TARGET) // �α��� ��û ��Ŷ ������ �ʰ� ���ǻ��¸� �����ϴ� �Ǽ������� ���ؼ� Ÿ�Ӿƿ� �׽�Ʈ ���̶�� �ǳʶ�
			continue;

		if (pPlayer->state_ == STATE::CS_CHAT_REQ_LOGIN_SENT)
			continue;

		if (curTime < pPlayer->lastRecvTime_ + g_delayLogin)
			continue;

		// delay login ��ŭ�� �ð��� �帣�� �α��� ��Ŷ ������
		SmartPacket sp = PACKET_ALLOC(Net);
		MAKE_CS_CHAT_REQ_LOGIN(pPlayer->pInfo_->accountNo, pPlayer->pInfo_->ID, pPlayer->pInfo_->Nick, pPlayer->pInfo_->SessionKey, sp);
		g_pChatDummy->SendPacket(pPlayer->sessionId_, sp);
		pPlayer->lastSendTime_ = timeGetTime();
		pPlayer->state_ = STATE::CS_CHAT_REQ_LOGIN_SENT;
		++g_replyWait;
	}

	curTime = timeGetTime();
	for (auto pair : g_chatLoginPlayerMap)
	{
		Player* pPlayer = pair.second;

		switch (pPlayer->state_)
		{
		case STATE::CS_CHAT_RES_LOGIN_RECV:
		case STATE::CS_CHAT_RES_SECTOR_MOVE_RECV:
		case STATE::CS_CHAT_RES_MESSAGE_RECV:
		{
			// ����Ŭ�󿡼� Disconnect�� ȣ���ص� IOCP���� �񵿱������� ���� ������ ���� ������ Ŭ�� ���� Map�� �����Ҽ�����
			if ((pPlayer->field_ & DISCONNECT_TARGET) == DISCONNECT_TARGET)
				continue;

			// ���߷α��� ����, �α��� Ÿ�Ӿƿ� �׽�Ʈ Ŭ��� �����Ǿ��ٸ� �� �̻� �޽����� ������ �ʴ´�
			if ((pPlayer->field_ & (DUPLICATE_TARGET | LOGIN_TIMOUT_TARGET)) != 0)
				continue;

			// Ȯ�� ����� �޽����� ������� ������ �÷��̾ ���ؼ� DELAY_ACTION��ŭ�� �ð��� �귵���� üũ�� �귵���� �޽��� ����
			if ((pPlayer->field_ & RESERVE_SEND_MESSAGE) == RESERVE_SEND_MESSAGE)
			{
				if (curTime < pPlayer->lastRecvTime_ + g_delayAction)
					break;

				// ���� �޽��� ����� ������
				SendRandomChatMessage(pPlayer);
				pPlayer->lastSendTime_ = timeGetTime();
				pPlayer->field_ &= (~RESERVE_SEND_MESSAGE);
				++g_replyWait;
				break;
			}

			// RAND_CONNTENTS�� ��÷�̴��� Ȯ������� ��÷�̶�� �޽��� ������ ����, DEALY_ACTION��ŭ�� �ð��� �帣�� �޽��� ����
			if (rand() % 100 < g_randContents)
				pPlayer->field_ |= RESERVE_SEND_MESSAGE;

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

		if ((pPlayer->field_ & LOGIN_TIMOUT_TARGET) == LOGIN_TIMOUT_TARGET) // ���������� ���� Ÿ�Ӿƿ� ���� ��� Ŭ���� ��Ʈ��Ʈ�� ������ �ʾƾ��Ѵ�
			break;

		// ä�ü��� �α��� ���������� 30�ʿ� �ѹ� ��Ʈ��Ʈ ������
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
		pPlayer->bDuplicateNew_ = false;
		pPlayer->sessionId_ = pJob->sessionId_;
		pPlayer->lastRecvTime_ = timeGetTime();
		pPlayer->state_ = STATE::LOGIN_CONNECT_SUCCESS;
		pPlayer->field_ = INITIAL_FIELD;

		do
		{
			// ���߷α��� ��尡 �����հ� ���ÿ� Ȯ���� �������� ������ ���߷α��� ���� X
			if (!(g_bDuplicateLoginOn && (rand() % 100 < RAND_DUPLICATE_LOGIN)))
			{
				pPlayer->pInfo_ = g_NonConnectedStack.top();
				g_NonConnectedStack.pop();
				break;
			}

			// Disconnect ����, Ÿ�Ӿƿ� �׽�Ʈ,���߷α��� ���� ���� �������� ��� �׽�Ʈ ��� ������ �ƴ� �����߿��� �����Ѵ�
			const auto& iter = std::find_if(g_chatLoginPlayerMap.begin(), g_chatLoginPlayerMap.end(), [](auto pair) {return IS_VALID_FIELD_FOR_TEST(pair.second->field_); });
			if (iter == g_chatLoginPlayerMap.end())
			{
				pPlayer->pInfo_ = g_NonConnectedStack.top();
				g_NonConnectedStack.pop();
				break;
			}

			// �̹� ä�ü����� �α����ؼ� ���߷α��� �׽�Ʈ ������� ������ ������ ���� �Ӽ��ʵ弳�� �� �ش� ������ ������ ���� �α��� ������ �����ϴ� �������� ����
			iter->second->field_ |= DUPLICATE_TARGET;
			pPlayer->pInfo_ = iter->second->pInfo_;
			++g_duplicateLoginTPS;
			pPlayer->bDuplicateNew_ = true;
		} while (0);

		g_loginPlayerMap.insert(std::make_pair(pJob->sessionId_, pPlayer));
		++g_loginConnectTotal;
		break;
	}
	case JOBTYPE::RECV_MESSAGE:
	{
		--g_replyWait;
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
		pPlayer->field_ |= DISCONNECT_TARGET;
		g_pLoginDummy->Disconnect(pJob->sessionId_);
		break;
	}
	case JOBTYPE::ON_RELEASE:
	{
		auto iter = g_loginPlayerMap.find(pJob->sessionId_);
		Player* pPlayer = iter->second;
		if (pPlayer->state_ == STATE::LOGIN_TRY_DISCONNECT) // �����޽����� �����ؼ� Ŭ�󿡼� ���������� Disconnect �� ���
		{
			pPlayer->state_ = STATE::CHAT_CONNECT_TRY;
			g_moveQ.push(pPlayer);
			g_pChatDummy->Connect(false, &pPlayer->sockAddr_);
		}
		else // �α��� ���� ���� �޽����� �������Լ� ���� ���޴µ� ������ ������
		{
			++g_loginDownClient;
			++g_disconnectedNum;
			g_NonConnectedStack.push(pPlayer->pInfo_);
			RetMemoryToPool(g_playerPool, pPlayer);
		}
		g_loginPlayerMap.erase(iter);
		break;
	}
	case JOBTYPE::CONNECT_FAIL:
	{
		++g_loginConnectFail;
		break;
	}
	case JOBTYPE::ALL_RECONNECT_FAIL:
	{
		++g_disconnectedNum;
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
		pPlayer->field_ &= ~DISCONNECT_TARGET;

		// Ÿ�Ӿƿ� ������ ��� ����Ȯ�� ������ Ÿ�Ӿƿ� Ŭ��� ����
		// Connect ���������� Duplicate Target�� ���� ����
		// �ٸ� �α��� ���� �׽�ƮŬ��� ���߷α��ο��� ���߿� ������ Ŭ�� �����Ǹ� �α��� ������ Ÿ�Ӿƿ����� �����κ��� ���� ������ �����Ƿ� bDuplicateNew�� false�� Ŭ�� ���ؼ� ����
		if (g_bTestTimeOut && !pPlayer->bDuplicateNew_ && (rand() % 100 < RAND_TIMEOUT))
		{
			// 50 / 50�� Ȯ���� ���� Ÿ�Ӿƿ����� �α��� Ÿ�Ӿƿ����� ����
			if (rand() % 2 == 0)
			{
				pPlayer->field_ |= SESSION_TIMEOUT_TARGET;
				++g_sessionTimeOutTPS;
			}
			else
			{
				pPlayer->field_ |= LOGIN_TIMOUT_TARGET;
				++g_loginTimeOutTPS;
			}
		}

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
		++g_disconnectedNum;
		Player* pPlayer;
		auto connectIter = g_chatConnectPlayerMap.find(pJob->sessionId_);
		if (connectIter != g_chatConnectPlayerMap.end())
		{
			--g_chatConnectNum;
			pPlayer = connectIter->second;
			g_chatConnectPlayerMap.erase(connectIter);
			g_NonConnectedStack.push(pPlayer->pInfo_);

			pPlayer = connectIter->second;
			if ((pPlayer->field_ & SESSION_TIMEOUT_TARGET) == SESSION_TIMEOUT_TARGET)
			{
				RefreshSessionTimoutTotal(timeGetTime(), pPlayer->lastRecvTime_);
				--g_sessionTimeOutTPS;
			}
			else // login���� ���� ���¿����� �׽�Ʈ Ŭ�� �ƴ϶�� ���� ������ Disconnect�� ���� ȣ������� ���⿡ STATE::CHAT_TRY_DISCONNECT�� ������ ����
			{
				++g_chatDownClient;
			}
		}
		else
		{
			--g_chatLoginNum;
			auto loginIter = g_chatLoginPlayerMap.find(pJob->sessionId_);
			if (loginIter == g_chatLoginPlayerMap.end())
			{
				LOG(L"ERROR", ERR, TEXTFILE, L"chat Session Is Release But Neither Connect Or Login Map");
				__debugbreak();
			}
			pPlayer = loginIter->second;
			g_chatLoginPlayerMap.erase(loginIter);

			if ((pPlayer->field_ & DUPLICATE_TARGET) == DUPLICATE_TARGET)
			{
				--g_duplicateLoginTPS;
			}
			else
			{
				g_NonConnectedStack.push(pPlayer->pInfo_);
				if ((pPlayer->field_ & LOGIN_TIMOUT_TARGET) == LOGIN_TIMOUT_TARGET)
				{
					RefreshLoginTimoutTotal(timeGetTime(),pPlayer->lastRecvTime_);
					--g_loginTimeOutTPS;
				}
				else if (pPlayer->state_ != STATE::CHAT_TRY_DISCONNECT)
				{
					++g_chatDownClient;
				}
			}
		}

		RetMemoryToPool(g_playerPool, pPlayer);
		break;
	}
	case JOBTYPE::CONNECT_FAIL:
	{
		++g_chatConnectFail;
		break;
	}

	case JOBTYPE::ALL_RECONNECT_FAIL: //  ä�ü������� �������� ����Ƚ�� ���н� 
	{
		++g_disconnectedNum;
		Player* pPlayer = g_moveQ.front();
		g_moveQ.pop();
		if (pPlayer->bDuplicateNew_)
		{
			++g_duplicateConnectFail;
		}

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
	// ���߷α��� : ��������� ���߷α��� ������� ������� �𸣹Ƿ� ���� �޽����� �͵� �̻����� ����.
	// Ÿ�Ӿƿ�(����) : CONNECT ������ �������ΰ� �����ǹǷ� �޽����� �����ؼ��� �ȵ�
	// Ÿ�Ӿƿ�(�α���) : CONNECT ������ �������ΰ� �����ǰ� ������ ���ؼ� �α��α����� �����ϱ� ������ RES_SECTOR_MOVE Ȥ�� RES_CHAT_MESSAGE�� �����ؼ��� �ȵ�
	auto iter = g_chatConnectPlayerMap.find(sessionID);
	if (iter == g_chatConnectPlayerMap.end())
	{
		iter = g_chatLoginPlayerMap.find(sessionID);
		if (iter == g_chatLoginPlayerMap.end())
			__debugbreak();
	}

	Player* pPlayer = iter->second;

	WORD type;
	*pPacket >> type;
	switch ((en_PACKET_TYPE)type)
	{
	case en_PACKET_CS_CHAT_RES_LOGIN:
	{
		pPlayer->lastRecvTime_ = timeGetTime();
		CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);

		// RES_MESSAGE�� ��쿡�� Disconnect�� ȣ���ϰ� IOCnt�� 0�̵Ǵ� ���̿� �ٸ� �������� Ŭ��κ��� �� �޽����� MessageQ�� Enqueue�Ǿ���� ���ɼ��� �ձ⶧����
		if (pPlayer->state_ == STATE::CHAT_TRY_DISCONNECT)
		{
			LOG(L"ERROR", ERR, TEXTFILE, L"Call Disconnect But Recv RES_LOGIN");
			__debugbreak();
		}

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
		pPlayer->field_ |= LOGIN_ACTIVATE_AT_CHAT;
		g_chatConnectPlayerMap.erase(iter);
		auto ret = g_chatLoginPlayerMap.insert(std::make_pair(sessionID, pPlayer));
		if (!ret.second)
		{
			__debugbreak();
		}
		--g_chatConnectNum;
		++g_chatLoginNum;
		--g_replyWait;
		break;
	}
	case en_PACKET_CS_CHAT_RES_SECTOR_MOVE:
	{
		pPlayer->lastRecvTime_ = timeGetTime();
		CaculateRTT(pPlayer->lastRecvTime_, pPlayer->lastSendTime_);

		// RES_MESSAGE�� ��쿡�� Disconnect�� ȣ���ϰ� IOCnt�� 0�̵Ǵ� ���̿� �ٸ� �������� Ŭ��κ��� �� �޽����� MessageQ�� Enqueue�Ǿ���� ���ɼ��� �ձ⶧����
		if (pPlayer->state_ == STATE::CHAT_TRY_DISCONNECT)
		{
			LOG(L"ERROR", ERR, TEXTFILE, L"Call Disconnect But Recv RES_SECTOR_MOVE");
			__debugbreak();
		}

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
		--g_replyWait;
		break;
	}

	case en_PACKET_CS_CHAT_RES_MESSAGE: 
	{
		if((pPlayer->field_ & REGISTER_AT_SECTOR) == 0)
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
			--g_replyWait;
		}
		else
			return;
		break;
	}
	default:
		__debugbreak();
		break;
	}

	// �޽��� ���Ž� ���� Ȯ���� �׽�Ʈ ��� ���� Ȥ�� DISCONNECT
	// ���߷α���, Ÿ�Ӿƿ�, ���� �׽�Ʈ���� Ŭ��� ���Ƿ� ��������, ��κ��� �����̱� ������ ������� �������� debugbreak�� �ɸ��� ���� ������ ���� CHAT_RES_MESSAGE�� �޴°�찡 �̿� �ش��
	if ((pPlayer->field_ & (DUPLICATE_TARGET | DISCONNECT_TARGET | SESSION_TIMEOUT_TARGET | LOGIN_TIMOUT_TARGET)) != 0)
		return;

	if (rand() % 100 < g_randDisconnect)
	{
		pPlayer->field_ |= DISCONNECT_TARGET;
		pPlayer->state_ = STATE::CHAT_TRY_DISCONNECT;
		g_pChatDummy->Disconnect(sessionID);
	}
}

void SendRandomChatMessage(Player* pPlayer)
{
	SmartPacket sp = PACKET_ALLOC(Net);
	if (((pPlayer->field_ & REGISTER_AT_SECTOR) == 0) || rand() % 5 < 1) // ���Ϳ� ����� �ȵ������� ������ �����̵� ������, �װԾƴϸ� Ȯ��������
	{
		pPlayer->sectorX_ = rand() % 49 + 1;
		pPlayer->sectorY_ = rand() % 49 + 1;
		MAKE_CS_CHAT_REQ_SECTOR_MOVE(pPlayer->pInfo_->accountNo, pPlayer->sectorX_, pPlayer->sectorY_, sp);
		pPlayer->state_ = STATE::CS_CHAT_REQ_SECTOR_MOVE_SENT;
		pPlayer->field_ |= REGISTER_AT_SECTOR; // ���⼭ �̸�����ؾ� �׻��̿� RES_MESSAGE �ٸ� ������ ���� �޽ð� ������ ���Ϳ��� ����� �Ǿ����� ������ ä�ø޽����� �޾Ѵٴ� ���� ������ �Ȼ���
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
