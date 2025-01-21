#include <cstdio>
#include <queue>
#include "Lyrics.h"

LyRics LyRics::lyricsArr[LyRics::maxSize];

void LyRics::Init(const WCHAR* pFileName)
{
	static bool bFirst = true;
	if (!bFirst) return;
	bFirst = false;

	FILE* pFile;
	if (0 != _wfopen_s(&pFile, pFileName, L"r"))
		__debugbreak();

	fseek(pFile, 0, SEEK_END);
	int fileSize = ftell(pFile);
	fseek(pFile, 0, SEEK_SET);

	WCHAR* pArr = new WCHAR[fileSize / 2];
	if (1 != fread_s(pArr, fileSize, fileSize, 1, pFile))
		__debugbreak();

	WCHAR* pTemp = pArr;
	WCHAR* cnt = pArr;
	int numOfLyrics = 0;

	int max = 0;
	std::queue<const WCHAR*> Q;
	for (int i = 0; i < LyRics::maxSize; ++i)
	{
		pTemp = wcstok_s(cnt, L"\r\n", &cnt);
		lyricsArr[i].len = (WORD)wcslen(pTemp);
		wcscpy_s(lyricsArr[i].pLyrics, MAX_PATH, pTemp);
	}

	delete[] pArr;
	return;
}
