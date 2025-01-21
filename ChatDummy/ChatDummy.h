#pragma once
#include "NetClient.h"

class ChatDummy : public NetClient
{
public:
	ChatDummy(const BOOL bAutoReconnect, const LONG autoReconnectCnt, const LONG autoReconInterval, const WCHAR* pConfigFile);
	BOOL Start();
	virtual void OnRecv(ULONGLONG id, SmartPacket& sp) override;
	virtual void OnError(ULONGLONG id, int errorType, Packet* pRcvdPacket) override;
	virtual void OnConnect(ULONGLONG id) override;
	virtual void OnRelease(ULONGLONG id) override;
	virtual void OnConnectFailed(ULONGLONG id) override;
	virtual void OnAutoResetAllFailed() override;

	const WCHAR* pConfigFile_;
};
