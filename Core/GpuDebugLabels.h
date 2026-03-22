#pragma once

// vcxproj のプリプロセッサで ENABLE_GPU_DEBUG_LABELS=1 を付与（x64 Debug/Release 推奨）
#ifndef ENABLE_GPU_DEBUG_LABELS
#if defined(_DEBUG)
#define ENABLE_GPU_DEBUG_LABELS 1
#else
#define ENABLE_GPU_DEBUG_LABELS 0
#endif
#endif

#include <d3d12.h>

#if ENABLE_GPU_DEBUG_LABELS
#ifndef USE_PIX
#define USE_PIX
#endif
#include <pix3.h> // WinPixEventRuntime（vcpkg: winpixevent）。PIXBeginEvent → RenderDoc ツリー表示
#endif

#if ENABLE_GPU_DEBUG_LABELS
#define GPU_SET_NAME(resource, wideName) \
	do { \
		if ((resource)) \
			(resource)->SetName(wideName); \
	} while (0)

#define GPU_CMD_BEGIN_EVENT(cmdList, r, g, b, wideName) \
	PIXBeginEvent( \
		(cmdList), \
		PIX_COLOR(static_cast<UINT8>(r), static_cast<UINT8>(g), static_cast<UINT8>(b)), \
		wideName)

#define GPU_CMD_END_EVENT(cmdList) PIXEndEvent(cmdList)
#else
#define GPU_SET_NAME(resource, wideName) ((void)0)
#define GPU_CMD_BEGIN_EVENT(cmdList, r, g, b, wideName) ((void)0)
#define GPU_CMD_END_EVENT(cmdList) ((void)0)
#endif
