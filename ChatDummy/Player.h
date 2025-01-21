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
	DWORD lastSendTime_; // RTT ������
	DWORD lastHeartbeatSendtime_;
	STATE state_;
	const AccountInfo* pInfo_;
	bool bDisconnectCalled_;
	bool bAuthAtLoginServer_;
	bool bAuthAtChatServer_;
	bool bRegisterAtSector_;
	bool bWillSendmessage_; // DelayAction�ð��� ����ϸ� �޽����� ���� �����϶� ���� �÷���, �α��� ��Ŷ���� �������� �ʴ´�....
	WORD sectorX_;
	WORD sectorY_;
	const LyRics* pLastSentChat;
	SOCKADDR_IN sockAddr_;

};
