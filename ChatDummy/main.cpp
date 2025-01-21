#include <WinSock2.h>
#include <windows.h>
#include "LoginDummy.h"
#include "ChatDummy.h"
#include "Update.h"

LoginDummy* g_pLoginDummy;
ChatDummy* g_pChatDummy;

int main()
{
	g_pLoginDummy = new LoginDummy(true, 3, 100, L"LoginDummyConfig.txt");
	g_pChatDummy = new ChatDummy(true, 3, 100, L"ChatDummyConfig.txt");
	g_pLoginDummy->Start();
	g_pChatDummy->Start();
	UpdateLoop(20, 1, 30, 0, 80, 27, 1000, L"ID1.txt", L"ChatList.txt");
	return 0;
}