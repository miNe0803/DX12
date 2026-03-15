#include "App.h"
#include <Engine.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

int wmain(int argc, wchar_t** argv, wchar_t** envp)
{
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
        }
    }
#endif
	StartApp(TEXT("DirectX12“ü–å"));
	return 0;
}
