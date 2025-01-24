#include <WS2tcpip.h>
#include <locale>

#include "NetClient.h"
#include "Logger.h"
#include "Parser.h"

#include "Assert.h"
#include "NetClientSession.h"
#include "UpdateBase.h"

#pragma comment(lib,"LoggerMT.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib,"TextParser.lib")
#pragma comment(lib,"Winmm.lib")

template<typename T>
__forceinline T& IGNORE_CONST(const T& value)
{
	return const_cast<T&>(value);
}

bool WsaIoctlWrapper(SOCKET sock, GUID guid, LPVOID* pFuncPtr)
{
	DWORD bytes = 0;
	return SOCKET_ERROR != WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), pFuncPtr, sizeof(*pFuncPtr), &bytes, NULL, NULL);
}

void NetClient::ConnectProc(NetClientSession* pSession)
{
	InterlockedIncrement(&pSession->refCnt_);
	InterlockedAnd(&pSession->refCnt_, ~NetClientSession::RELEASE_FLAG);

	if (0 > setsockopt(pSession->sock_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0))
		__debugbreak();

	//SetLinger(pSession->sock_);
	//SetZeroCopy(pSession->sock_);
	pSession->Init(InterlockedIncrement(&ullIdCounter_) - 1, (short)(pSession - pSessionArr_));

	OnConnect(pSession->id_);
	RecvPost(pSession);
}

void NetClient::DisconnectProc(NetClientSession* pSession)
{
	// Release �� Session�� ����ȭ ���� ����
	for (LONG i = 0; i < pSession->lSendBufNum_; ++i)
	{
		Packet* pPacket = pSession->pSendPacketArr_[i];
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	LONG size = pSession->sendPacketQ_.GetSize();
	for (LONG i = 0; i < size; ++i)
	{
		Packet* pPacket = pSession->sendPacketQ_.Dequeue().value();
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	OnRelease(pSession->id_);
	// ���⼭���� �ٽ� Connect�� ���������� ������ ������ �� �Լ� ���ϴܿ� �;���
	DisconnectStack_.Push((short)(pSession - pSessionArr_));
	InterlockedDecrement(&lSessionNum_);
}

void CALLBACK ReconnectTimer(PVOID lpParam, BOOLEAN TimerOrWaitFired)
{
	static MYOVERLAPPED ReconnectOverlapped{ OVERLAPPED{},OVERLAPPED_REASON::RECONNECT };

	NetClient* pNetClient = (NetClient*)lpParam;
	NetClientSession* pSession = pNetClient->ReconnectQ_.Dequeue().value();
	PostQueuedCompletionStatus(pNetClient->hcp_, 2, (ULONG_PTR)pSession, &ReconnectOverlapped.overlapped);
}

unsigned __stdcall NetClient::IOCPWorkerThread(LPVOID arg)
{
#pragma warning(disable : 4244)
	srand(time(nullptr));
#pragma warning(default: 4244)
	NetClient* pNetClient = (NetClient*)arg;
	while (1)
	{
		MYOVERLAPPED* pOverlapped = nullptr;
		DWORD dwNOBT = 0;
		NetClientSession* pSession = nullptr;
		bool bContinue = false;
		bool bConnectSuccess = true;
		BOOL bGQCSRet = GetQueuedCompletionStatus(pNetClient->hcp_, &dwNOBT, (PULONG_PTR)&pSession, (LPOVERLAPPED*)&pOverlapped, INFINITE);
		do
		{
			if (!pOverlapped && !dwNOBT && !pSession)
				return 0;

			if (!bGQCSRet && pOverlapped)
			{
				DWORD errCode = WSAGetLastError();
				if (errCode != 0)
					LOG(L"ERROR", ERR, TEXTFILE, L"ConnectEx Failed ErrCode : %u", WSAGetLastError());

				if (pOverlapped->why == OVERLAPPED_REASON::CONNECT)
				{
					bContinue = true;
					closesocket(pSession->sock_);
					pNetClient->OnConnectFailed(pSession->id_); // ����ڰ� ���ϱ⿡ ���� �ٷ� �ٽð��� �ƴϸ� �׳� �ε��� ���ÿ� �������� ����

					if (pNetClient->bAutoReconnect_)
					{
						if (InterlockedDecrement(&pSession->reConnectCnt_) > 0)
						{
							HANDLE hTimer;
							pNetClient->ReconnectQ_.Enqueue(pSession);
							if (0 == CreateTimerQueueTimer(&hTimer, NULL, ReconnectTimer, (PVOID)pNetClient, pNetClient->autoReconnectInterval_, 0, WT_EXECUTEDEFAULT))
							{
								DWORD errCode = GetLastError();
								__debugbreak();
							}
						}
						else
						{
							pNetClient->DisconnectStack_.Push((short)(pSession - pNetClient->pSessionArr_)); // �Ϻη� �տ���
							pNetClient->OnAutoResetAllFailed();
						}
					}
					else
						pNetClient->DisconnectStack_.Push((short)(pSession - pNetClient->pSessionArr_));
					continue;
				}

				else
					break;
			}

			switch (pOverlapped->why)
			{
			case OVERLAPPED_REASON::SEND:
			{
				pNetClient->SendProc(pSession, dwNOBT);
				break;
			}
			case OVERLAPPED_REASON::RECV:
				if (!(bGQCSRet && dwNOBT == 0))
				{
					pNetClient->RecvProc(pSession, dwNOBT);
				}
				break;

			case OVERLAPPED_REASON::TIMEOUT:
				break;

			case OVERLAPPED_REASON::UPDATE:
			{
				((UpdateBase*)(pSession))->Update();
				bContinue = true;
				break;
			}

			case OVERLAPPED_REASON::POST:
				break;

			case OVERLAPPED_REASON::SEND_WORKER:
				break;

			case OVERLAPPED_REASON::CONNECT:
			{
				pNetClient->ConnectProc(pSession);
				break;
			}

			case OVERLAPPED_REASON::RECONNECT:
			{
				bContinue = true;
				pNetClient->ConnectPost(true, pSession, &pSession->sockAddrIn_); // ConnectPost���� ������ �õ��� IP�� �����.
				break;
			}

			default:
				__debugbreak();
			}

		} while (0);

		if (bContinue)
			continue;

		if (InterlockedDecrement(&pSession->refCnt_) == 0)
			pNetClient->ReleaseSession(pSession);
	}
	return 0;
}

bool NetClient::SetLinger(SOCKET sock)
{
	linger linger;
	linger.l_onoff = 1;
	linger.l_linger = 0;
	return setsockopt(sock, SOL_SOCKET, SO_LINGER, (char*)&linger, sizeof(linger)) == 0;
}

bool NetClient::SetZeroCopy(SOCKET sock)
{
	DWORD dwSendBufSize = 0;
	return setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&dwSendBufSize, sizeof(dwSendBufSize)) == 0;
}

bool NetClient::SetReuseAddr(SOCKET sock)
{
	DWORD option = 1;
	return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&option, sizeof(option)) == 0;
}

bool NetClient::SetClientBind(SOCKET sock)
{
	SOCKADDR_IN addr;
	ZeroMemory(&addr, sizeof(SOCKADDR_IN));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
	addr.sin_port = ::htons(0);

	if (bind(sock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		DWORD errCode = WSAGetLastError();
		__debugbreak();
		return false;
	}
	return true;
}

NetClient::NetClient(const BOOL bAutoReconnect, const LONG autoReconnectCnt, const LONG autoReconnectInterval, BOOL bUseMemberSockAddrIn, const WCHAR* pIP, const USHORT port, const DWORD iocpWorkerThreadNum,
	const DWORD cunCurrentThreadNum, const LONG maxSession, const BYTE packetCode, const BYTE packetFixedKey)
	:bAutoReconnect_{ bAutoReconnect }, autoReconnectCnt_{ autoReconnectCnt }, autoReconnectInterval_{ autoReconnectInterval }, IOCP_WORKER_THREAD_NUM_{ iocpWorkerThreadNum }, IOCP_ACTIVE_THREAD_NUM_{ cunCurrentThreadNum }, maxSession_{ maxSession },
	pSessionArr_{ new NetClientSession[maxSession] }, hIOCPWorkerThreadArr_{ new HANDLE[iocpWorkerThreadNum] }, hcp_{ CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, cunCurrentThreadNum) }
{
	timeBeginPeriod(1);

	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	SOCKET dummySock = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_LOG(WsaIoctlWrapper(dummySock, WSAID_CONNECTEX, (LPVOID*)&lpfnConnectExPtr_) == false, L"WSAIoCtl ConnectEx Failed");
	closesocket(dummySock);

	if (bUseMemberSockAddrIn)
	{
		// sockAddr_ �ʱ�ȭ, ConnectEx���� ���
		ZeroMemory(&sockAddr_, sizeof(sockAddr_));
		sockAddr_.sin_family = AF_INET;
		InetPtonW(AF_INET, pIP, &sockAddr_.sin_addr);
		sockAddr_.sin_port = htons(port);
	}

	Packet::PACKET_CODE = packetCode;
	Packet::FIXED_KEY = packetFixedKey;

	// IOCP �ڵ� �˻� 
	ASSERT_LOG(hcp_ == NULL, L"CreateIoCompletionPort Fail");
	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"Create IOCP OK!");

	// ���� 17��Ʈ�� ������ ������Ʈ�� 16�� ���ϰ� �Ǵ³����� ������� ū�׸��̴�.
	ASSERT_FALSE_LOG(CAddressTranslator::CheckMetaCntBits(), L"LockFree 17bits Over");

	for (int i = maxSession_ - 1; i >= 0; --i)
		DisconnectStack_.Push(i);

	for (DWORD i = 0; i < IOCP_WORKER_THREAD_NUM_; ++i)
	{
		hIOCPWorkerThreadArr_[i] = (HANDLE)_beginthreadex(NULL, 0, IOCPWorkerThread, this, CREATE_SUSPENDED, nullptr);
		ASSERT_ZERO_LOG(hIOCPWorkerThreadArr_[i], L"MAKE WorkerThread Fail");
	}
	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"MAKE IOCP WorkerThread OK Num : %u!", IOCP_WORKER_THREAD_NUM_);
}

//NetClient::NetClient(const BOOL bAutoReconnect, const LONG autoReconnectCnt, const LONG autoReconnectInterval, const WCHAR* pConfigFileName)
//	:bAutoReconnect_{ bAutoReconnect }, autoReconnectCnt_{ autoReconnectCnt }, autoReconnectInterval_{ autoReconnectInterval }
//{
//	timeBeginPeriod(1);
//	std::locale::global(std::locale(""));
//	char* pStart;
//	char* pEnd;
//
//	WSADATA wsaData;
//	WSAStartup(MAKEWORD(2, 2), &wsaData);
//
//	SOCKET dummySock = socket(AF_INET, SOCK_STREAM, 0);
//	ASSERT_LOG(WsaIoctlWrapper(dummySock, WSAID_CONNECTEX, (LPVOID*)&lpfnConnectExPtr_) == false, L"WSAIoCtl ConnectEx Failed");
//	closesocket(dummySock);
//
//	PARSER psr = CreateParser(pConfigFileName);
//	WCHAR ipStr[16];
//	GetValue(psr, L"BIND_IP", (PVOID*)&pStart, (PVOID*)&pEnd);
//	unsigned long long stringLen = (pEnd - pStart) / sizeof(WCHAR);
//	wcsncpy_s(ipStr, _countof(ipStr) - 1, (const WCHAR*)pStart, stringLen);
//	// Null terminated String ���� ������ InetPtonW��������
//	ipStr[stringLen] = 0;
//
//	// sockAddr_ �ʱ�ȭ, ConnectEx���� ���
//	ZeroMemory(&sockAddr_, sizeof(sockAddr_));
//	sockAddr_.sin_family = AF_INET;
//	InetPtonW(AF_INET, ipStr, &sockAddr_.sin_addr);
//
//	GetValue(psr, L"BIND_PORT", (PVOID*)&pStart, nullptr);
//	unsigned short SERVER_PORT = (unsigned short)_wtoi((LPCWSTR)pStart);
//	sockAddr_.sin_port = htons(SERVER_PORT);
//
//	GetValue(psr, L"IOCP_WORKER_THREAD", (PVOID*)&pStart, nullptr);
//	IGNORE_CONST(IOCP_WORKER_THREAD_NUM_) = (DWORD)_wtoi((LPCWSTR)pStart);
//
//	GetValue(psr, L"IOCP_ACTIVE_THREAD", (PVOID*)&pStart, nullptr);
//	IGNORE_CONST(IOCP_ACTIVE_THREAD_NUM_) = (DWORD)_wtoi((LPCWSTR)pStart);
//
//	GetValue(psr, L"IS_ZERO_BYTE_SEND", (PVOID*)&pStart, nullptr);
//	int bZeroByteSend = _wtoi((LPCWSTR)pStart);
//
//	GetValue(psr, L"SESSION_MAX", (PVOID*)&pStart, nullptr);
//	IGNORE_CONST(maxSession_) = _wtoi((LPCWSTR)pStart);
//
//	GetValue(psr, L"PACKET_CODE", (PVOID*)&pStart, nullptr);
//	Packet::PACKET_CODE = (unsigned char)_wtoi((LPCWSTR)pStart);
//
//	GetValue(psr, L"PACKET_KEY", (PVOID*)&pStart, nullptr);
//	Packet::FIXED_KEY = (unsigned char)_wtoi((LPCWSTR)pStart);
//	ReleaseParser(psr);
//
//	// IOCP �ڵ� ����
//	hcp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, IOCP_ACTIVE_THREAD_NUM_);
//	ASSERT_LOG(hcp_ == NULL, L"CreateIoCompletionPort Fail");
//	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"Create IOCP OK!");
//
//	// ���� 17��Ʈ�� ������ ������Ʈ�� 16�� ���ϰ� �Ǵ³����� ������� ū�׸��̴�.
//	ASSERT_FALSE_LOG(CAddressTranslator::CheckMetaCntBits(), L"LockFree 17bits Over");
//
//	pSessionArr_ = new NetClientSession[maxSession_];
//	for (int i = maxSession_ - 1; i >= 0; --i)
//		DisconnectStack_.Push(i);
//
//	// IOCP ��Ŀ������ ����(CREATE_SUSPENDED)
//	hIOCPWorkerThreadArr_ = new HANDLE[IOCP_WORKER_THREAD_NUM_];
//	for (DWORD i = 0; i < IOCP_WORKER_THREAD_NUM_; ++i)
//	{
//		hIOCPWorkerThreadArr_[i] = (HANDLE)_beginthreadex(NULL, 0, IOCPWorkerThread, this, CREATE_SUSPENDED, nullptr);
//		ASSERT_ZERO_LOG(hIOCPWorkerThreadArr_[i], L"MAKE WorkerThread Fail");
//	}
//	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"MAKE IOCP WorkerThread OK Num : %u!", IOCP_WORKER_THREAD_NUM_);
//}

NetClient::~NetClient()
{
	WSACleanup();
}

void NetClient::InitialConnect(SOCKADDR_IN* pSockAddrIn)
{
	while (true)
	{
		auto opt = DisconnectStack_.Pop();
		if (opt.has_value() == false)
			return;

		NetClientSession* pSession = pSessionArr_ + opt.value();
		ConnectPost(false, pSession, pSockAddrIn);
	}
}

bool NetClient::Connect(bool bRetry, SOCKADDR_IN* pSockAddrIn)
{
	auto opt = DisconnectStack_.Pop();
	if (opt.has_value() == false)
	{
		__debugbreak();
		return false;
	}

	NetClientSession* pSession = pSessionArr_ + opt.value();
	ConnectPost(bRetry, pSession, pSockAddrIn);
	return true;
}


void NetClient::SendPacket(ULONGLONG id, SmartPacket& sendPacket)
{
	NetClientSession* pSession = pSessionArr_ + NetClientSession::GET_SESSION_INDEX(id);
	long IoCnt = InterlockedIncrement(&pSession->refCnt_);

	// �̹� RELEASE �������̰ų� RELEASE�� ���
	if ((IoCnt & NetClientSession::RELEASE_FLAG) == NetClientSession::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->refCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE �Ϸ��� �ٽ� ���ǿ� ���� �ʱ�ȭ�� �Ϸ�Ȱ�� �� ��Ȱ��
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->refCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// ���ڵ�
	sendPacket->SetHeader<Net>();
	sendPacket->IncreaseRefCnt();
	pSession->sendPacketQ_.Enqueue(sendPacket.GetPacket());
	SendPost(pSession);
	if (InterlockedDecrement(&pSession->refCnt_) == 0)
		ReleaseSession(pSession);
}

void NetClient::Disconnect(ULONGLONG id)
{
	NetClientSession* pSession = pSessionArr_ + NetClientSession::GET_SESSION_INDEX(id);
	long IoCnt = InterlockedIncrement(&pSession->refCnt_);

	// RELEASE������ Ȥ�� ����Ϸ�
	if ((IoCnt & NetClientSession::RELEASE_FLAG) == NetClientSession::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->refCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE�� ��Ȱ����� �Ǿ�����
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->refCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// Disconnect 1ȸ ����
	if (InterlockedExchange((LONG*)&pSession->bDisconnectCalled_, TRUE) == TRUE)
	{
		if (InterlockedDecrement(&pSession->refCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// ���� �����޴ٸ� ���� ���ǿ� ���ؼ� RELEASE ���� ȣ����� ������������ ����ȴ�
	CancelIoEx((HANDLE)pSession->sock_, &pSession->sendOverlapped.overlapped);
	CancelIoEx((HANDLE)pSession->sock_, &pSession->recvOverlapped.overlapped);

	// CancelIoExȣ��� ���ؼ� RELEASE�� ȣ��Ǿ���� ������ �������� InterlockedIncrement ������ ȣ���� �ȵ� ��� ����û��
	if (InterlockedDecrement(&pSession->refCnt_) == 0)
		ReleaseSession(pSession);
}

bool NetClient::ConnectPost(bool bRetry, NetClientSession* pSession, SOCKADDR_IN* pSockAddrIn)
{
	if (!bRetry)
	{
		memcpy(&pSession->sockAddrIn_, pSockAddrIn, sizeof(SOCKADDR_IN));
		InterlockedExchange(&pSession->reConnectCnt_, autoReconnectCnt_);
	}

	// ���� ����, �ɼ� ������ IOCP�� ���
	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	SetClientBind(sock);
	SetZeroCopy(sock);
	//SetLinger(sock);

	if (NULL == CreateIoCompletionPort((HANDLE)sock, hcp_, (ULONG_PTR)pSession, 0))
	{
		DWORD errCode = WSAGetLastError();
		__debugbreak();
	}

	pSession->sock_ = sock;
	ZeroMemory(&pSession->connectOverlapped.overlapped, sizeof(WSAOVERLAPPED));
	pSession->connectOverlapped.why = OVERLAPPED_REASON::CONNECT;
	BOOL bConnectExRet = lpfnConnectExPtr_(sock, (SOCKADDR*)pSockAddrIn, sizeof(SOCKADDR_IN), nullptr, 0, NULL, &pSession->connectOverlapped.overlapped);
	if (bConnectExRet == FALSE)
	{
		DWORD errCode = WSAGetLastError();
		if (errCode == WSA_IO_PENDING)
			return true;

		closesocket(sock);
		OnConnectFailed(pSession->id_);
		if (bAutoReconnect_)
		{
			if (InterlockedDecrement(&pSession->reConnectCnt_) > 0)
			{
				HANDLE hTimer;
				if (0 == CreateTimerQueueTimer(&hTimer, NULL, ReconnectTimer, (PVOID)this, 100, 0, WT_EXECUTEDEFAULT))
				{
					DWORD errCode = GetLastError();
					__debugbreak();
				}
			}
		}
		else
			DisconnectStack_.Push((short)(pSession - pSessionArr_));

		LOG(L"ERROR", ERR, TEXTFILE, L"ConnectEx ErrCode : %u", errCode);
		__debugbreak();
		return false;
	}
	else
	{
		__debugbreak();
	}
	return true;
}


BOOL NetClient::SendPost(NetClientSession* pSession)
{
	DWORD dwBufferNum;
	while (1)
	{
		if (pSession->sendPacketQ_.GetSize() <= 0)
			return FALSE;

		// ���� ���� TRUE�� �ٲ۴�. ���� TRUE���ٸ� ��ȯ���� TRUE�ϰ��̸� �׷��ٸ� ���� SEND �������̱� ������ �׳� ����������
		// �� ���ǹ��� ��ġ�� ���Ͽ� Out�� �ٲ��� ���������� ����ȴ�.
		// ������ SendPost ������ü�� Send�Ϸ����� �������� ��쿡�� in�� ��ġ�� SendPacket���� ���ؼ� �ٲ���� �ִ�.
		// iUseSize�� ���ϴ� ���������� DirectDequeueSize�� ���� �޶������ִ�.
		if (InterlockedExchange((LONG*)&pSession->bSendingInProgress_, TRUE) == TRUE)
			return TRUE;

		// SendPacket���� in�� �Űܼ� UseSize�� 0���� Ŀ���������� Send�Ϸ������� �����ؼ� Out�� �ű�� �÷��� ���� Recv�Ϸ����� �����尡 ���� SendPost�� ������ �÷��׸� �����Ѱ�� UseSize�� 0�̳��´�.
		// ���⼭ flag�� �ٽ� FALSE�� �ٲپ����� �ʾƼ� ����߻�
		dwBufferNum = pSession->sendPacketQ_.GetSize();

		if (dwBufferNum <= 0)
			InterlockedExchange((LONG*)&pSession->bSendingInProgress_, FALSE);
		else
			break;
	}

	WSABUF wsa[50];
	DWORD i;
	for (i = 0; i < 50 && i < dwBufferNum; ++i)
	{
#pragma warning(disable : 26815)
		Packet* pPacket = pSession->sendPacketQ_.Dequeue().value();
#pragma warning(default: 26815)
		wsa[i].buf = (char*)pPacket->pBuffer_;
		wsa[i].len = pPacket->GetUsedDataSize() + sizeof(Packet::NetHeader);
		pSession->pSendPacketArr_[i] = pPacket;
	}

	InterlockedExchange(&pSession->lSendBufNum_, i);
	InterlockedIncrement(&pSession->refCnt_);
	ZeroMemory(&(pSession->sendOverlapped.overlapped), sizeof(WSAOVERLAPPED));
	pSession->sendOverlapped.why = OVERLAPPED_REASON::SEND;
	int iSendRet = WSASend(pSession->sock_, wsa, i, nullptr, 0, &pSession->sendOverlapped.overlapped, nullptr);
	if (iSendRet == SOCKET_ERROR)
	{
		DWORD dwErrCode = WSAGetLastError();
		if (dwErrCode == WSA_IO_PENDING)
		{
			if (pSession->bDisconnectCalled_ == TRUE)
			{
				CancelIoEx((HANDLE)pSession->sock_, &pSession->sendOverlapped.overlapped);
				return FALSE;
			}
			return TRUE;
		}

		InterlockedDecrement(&(pSession->refCnt_));
		if (dwErrCode == WSAECONNRESET)
			return FALSE;

		LOG(L"Disconnect", ERR, TEXTFILE, L"Disconnected By ErrCode : %u", dwErrCode);
		return FALSE;
	}
	return TRUE;
}

BOOL NetClient::RecvPost(NetClientSession* pSession)
{
	WSABUF wsa[2];
	wsa[0].buf = pSession->recvRB_.GetWriteStartPtr();
	wsa[0].len = pSession->recvRB_.DirectEnqueueSize();
	wsa[1].buf = pSession->recvRB_.Buffer_;
	wsa[1].len = pSession->recvRB_.GetFreeSize() - wsa[0].len;

	ZeroMemory(&pSession->recvOverlapped.overlapped, sizeof(WSAOVERLAPPED));
	pSession->recvOverlapped.why = OVERLAPPED_REASON::RECV;
	DWORD flags = 0;
	InterlockedIncrement(&pSession->refCnt_);
	int iRecvRet = WSARecv(pSession->sock_, wsa, 2, nullptr, &flags, &pSession->recvOverlapped.overlapped, nullptr);
	if (iRecvRet == SOCKET_ERROR)
	{
		DWORD dwErrCode = WSAGetLastError();
		if (dwErrCode == WSA_IO_PENDING)
		{
			if (pSession->bDisconnectCalled_ == TRUE)
			{
				CancelIoEx((HANDLE)pSession->sock_, &pSession->recvOverlapped.overlapped);
				return FALSE;
			}
			return TRUE;
		}

		InterlockedDecrement(&(pSession->refCnt_));
		if (dwErrCode == WSAECONNRESET)
			return FALSE;

		LOG(L"Disconnect", ERR, TEXTFILE, L"Disconnected By ErrCode : %u", dwErrCode);
		return FALSE;
	}
	return TRUE;
}

void NetClient::ReleaseSession(NetClientSession* pSession)
{
	if (InterlockedCompareExchange(&pSession->refCnt_, NetClientSession::RELEASE_FLAG | 0, 0) != 0)
		return;

	// Release �� Session�� ����ȭ ���� ����
	for (LONG i = 0; i < pSession->lSendBufNum_; ++i)
	{
		Packet* pPacket = pSession->pSendPacketArr_[i];
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	LONG size = pSession->sendPacketQ_.GetSize();
	for (LONG i = 0; i < size; ++i)
	{
		Packet* pPacket = pSession->sendPacketQ_.Dequeue().value();
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	closesocket(pSession->sock_);
	OnRelease(pSession->id_);
	// ���⼭���� �ٽ� Connect�� ���������� ������ ������ �� �Լ� ���ϴܿ� �;���
	DisconnectStack_.Push((short)(pSession - pSessionArr_));
	InterlockedDecrement(&lSessionNum_);
}


void NetClient::RecvProc(NetClientSession* pSession, int numberOfBytesTransferred)
{
	using NetHeader = Packet::NetHeader;
	pSession->recvRB_.MoveInPos(numberOfBytesTransferred);
	while (1)
	{
		Packet::NetHeader header;
		if (pSession->recvRB_.Peek((char*)&header, sizeof(NetHeader)) == 0)
			break;

		if (header.code_ != Packet::PACKET_CODE)
		{
			LOG(L"RecvError", ERR, TEXTFILE, L"Network Header Invalid : %d", header.code_);
			Disconnect(pSession->id_);
			return;
		}

		if (pSession->recvRB_.GetUseSize() < sizeof(NetHeader) + header.payloadLen_)
		{
			if (header.payloadLen_ > BUFFER_SIZE)
			{
				LOG(L"RecvError", ERR, TEXTFILE, L"Network Header payloadLen(%d) is Larger Than RBSize(%d)", header.payloadLen_, BUFFER_SIZE);
				Disconnect(pSession->id_);
				return;
			}
			break;
		}

		pSession->recvRB_.MoveOutPos(sizeof(NetHeader));
		SmartPacket sp = PACKET_ALLOC(Net);
		pSession->recvRB_.Dequeue(sp->GetPayloadStartPos<Net>(), header.payloadLen_);
		sp->MoveWritePos(header.payloadLen_);
		memcpy(sp->pBuffer_, &header, sizeof(Packet::NetHeader));

		// ���ڵ��� üũ�� Ȯ��
		if (sp->ValidateReceived() == false)
		{
			LOG(L"RecvError", ERR, TEXTFILE, L"RecvPacket Decoding Invalid");
			Disconnect(pSession->id_);
			return;
		}

		pSession->lastRecvTime = GetTickCount64();
		OnRecv(pSession->id_, sp);
	}
	RecvPost(pSession);
}

void NetClient::SendProc(NetClientSession* pSession, DWORD dwNumberOfBytesTransferred)
{
	LONG sendBufNum = InterlockedExchange(&pSession->lSendBufNum_, 0);
	for (LONG i = 0; i < sendBufNum; ++i)
	{
		Packet* pPacket = pSession->pSendPacketArr_[i];
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	InterlockedExchange((LONG*)&pSession->bSendingInProgress_, FALSE);
	SendPost(pSession);
}

