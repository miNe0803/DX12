#pragma once

#include <Windows.h>
#include <cstdarg>
#include <cstdio>

// VS「出力」ウィンドウ（出力元: デバッグ）に表示。GUI アプリで printf は見えないため使用する。
inline void DebugLog(const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	OutputDebugStringA(buf);
}
