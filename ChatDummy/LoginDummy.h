#pragma once
#include "NetClient.h"
#include "ChatDummy.h"

class LoginDummy : public NetClient
{
public:
	LoginDummy(const BOOL bAutoReconnect, const LONG autoReconnectCnt, const LONG autoReconnectInterval, const WCHAR* pConfigFile);
	BOOL Start();
	~LoginDummy();
	virtual void OnRecv(ULONGLONG id, SmartPacket& sp) override;
	virtual void OnError(ULONGLONG id, int errorType, Packet* pRcvdPacket) override;
	virtual void OnConnect(ULONGLONG id) override;
	virtual void OnRelease(ULONGLONG id) override;
	virtual void OnConnectFailed(ULONGLONG id) override;
	virtual void OnAutoResetAllFailed() override;
	const WCHAR* pConfigFile_;
	ChatDummy* pChatDummy_;
	int LoginConnectFailed_ = 0;
};
