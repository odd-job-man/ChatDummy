#include <windows.h>
#include "Monitor.h"
#include <stdio.h>
#include <strsafe.h>

extern int g_maxSession;
extern int g_needToTryConnect;
extern int g_loginDownClient;
extern int g_loginConnectFail;
extern int g_chatDownClient;
extern int g_chatConnectFail;

extern int g_loopNum;

extern DWORD g_rttMax;
extern DWORD g_rttNum;
extern ULONGLONG g_rttTotal;
extern float g_rttAvg;

char g_StartTimeArr[MAX_PATH];
char g_nowModeArr[MAX_PATH];

extern bool g_bPlay;
extern bool g_bConnectStop;
extern bool g_bDuplicateLoginOn;
extern bool g_bAttackOn;
//extern bool g_bTestTimeOut;

static char* g_pEndOfNowMode;
static size_t g_RemainingLengthExceptNodeMode;

void MonitorInit()
{
	SYSTEMTIME sysTime;
	GetLocalTime(&sysTime);
	sprintf_s(g_StartTimeArr, MAX_PATH, "StartTime : %2d.%2d.%2d %2d:%2d:%2d", sysTime.wYear, sysTime.wMonth, sysTime.wDay, sysTime.wHour, sysTime.wMinute, sysTime.wSecond);
	StringCchCopyExA(g_nowModeArr, MAX_PATH, "Now Mode : ", &g_pEndOfNowMode, &g_RemainingLengthExceptNodeMode, 0);
}

void MakeNowMode()
{
	char* pTemp = g_pEndOfNowMode;
	size_t len = g_RemainingLengthExceptNodeMode;

	if (g_bPlay)
		StringCchCopyExA(pTemp, len, "PLAY ", &pTemp, &len, 0);
	else
		StringCchCopyExA(pTemp, len, "STOP ", &pTemp, &len, 0);

	if(g_bConnectStop)
		StringCchCopyExA(pTemp, len, "| CONNECT STOP ", &pTemp, &len, 0);

	if(g_bAttackOn)
		StringCchCopyExA(pTemp, len, "| ATTACK ON", &pTemp, &len, 0);

	//if(g_bTestTimeOut)
	//	StringCchCopyExA(pTemp, len, "| ATTACK ON", &pTemp, &len, 0);
}

void Monitor()
{
	printf(
		"%s"
		"============================================================\n"
		"Now MODE : %s"
		"------------------------------------------------------------\n"
		"S : STOP / PLAY  |  A : CONNECT STOP  |  W : DUPLICATE LOGIN ON / OFF | T : ATTACK ON / OFF  |  Q : QUIT\n"
		"------------------------------------------------------------\n"
		"Client Total      : %d\n"
		"------------------------------------------------------------\n"
		"Disconnect        : %d\n"
		"------------------------------------------------------------\n"
		"IN Chat Server    : %d\n"
		"  ConnectTotal    : %d\n"
		"  Connect / Login : %d / %d\n"
		"  ConnectFail     : %d\n"
		"  Chat DownClient : %d\n"
		"------------------------------------------------------------\n"
		"ReplyWait         : %d\n"
		"------------------------------------------------------------\n",
		g_StartTimeArr,
		g_nowModeArr,
		g_maxSession,
		g_needToTryConnect
	);
}
