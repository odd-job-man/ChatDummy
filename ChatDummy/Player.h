#pragma once
#include <windows.h>
#include "CurrentServerType.h"
#include "State.h"
#include "AccountInfo.h"


struct LyRics;

struct Player
{
	static constexpr int ID_LEN = 20;
	static constexpr int NICK_NAME_LEN = 20;
	static constexpr int SESSION_KEY_LEN = 64;
	static inline int MAX_PLAYER_NUM = 5000;
	static inline Player* pChatPlayerArr;

	SERVERTYPE serverType_;
	ULONGLONG sessionId_;
	DWORD lastRecvTime_;
	DWORD lastSendTime_; // RTT 측정용
	DWORD lastHeartbeatSendtime_;
	STATE state_;
	const AccountInfo* pInfo_;
	bool bDisconnectCalled_;
	bool bAuthAtLoginServer_;
	bool bAuthAtChatServer_;
	bool bRegisterAtSector_;
	bool bWillSendmessage_; // DelayAction시간이 경과하면 메시지를 보낼 예정일때 쓰는 플래그, 로그인 패킷과는 연관되지 않는다....
	WORD sectorX_;
	WORD sectorY_;
	const LyRics* pLastSentChat;
	SOCKADDR_IN sockAddr_;

};
