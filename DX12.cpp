#include "framework.h"
#include "App.h"
#include <wrl.h>
#include <Shlwapi.h>

// DirectX 12 開発に必要なヘッダー
#include <d3d12.h>

#pragma comment(lib, "Shlwapi.lib")

using Microsoft::WRL::ComPtr;

/**
 * アプリケーションのエントリポイント
 * Windows OS から最初に呼び出される関数です。
 */
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    // 未使用パラメータの警告を回避
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // --- [0] 作業ディレクトリを exe と同じフォルダに ---
    // assets 等の相対パスが exe 基準で解決されるようにする
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
    {
        PathRemoveFileSpecW(exePath);
        SetCurrentDirectoryW(exePath);
    }

    // --- [1] デバッグレイヤーの有効化 ---
    // PIX は GPU キャプチャ時に独自のフックを入れるため、デバッグレイヤーと併用すると
    // 二重検証・タイミング変化で ERROR が増えたり、PIX 単独では問題ない経路で落ちたりしやすい。
    // RenderDoc はフック方式が異なり併用しやすい。PIX 用に環境変数 DX12_DISABLE_DEBUG_LAYER=1 で無効化可能。
#if defined(_DEBUG)
    {
        wchar_t ev[8]{};
        const DWORD n = GetEnvironmentVariableW(L"DX12_DISABLE_DEBUG_LAYER", ev, 8u);
        const bool disableLayer = (n > 0 && ev[0] == L'1');
        if (!disableLayer)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
        }
    }
#endif

    // --- [2] COMライブラリの初期化 ---
    // AssimpLoaderやDirectXTexで使用するWIC（画像読み込み）のために必要です。
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        return -1;
    }

    // --- [3] アプリケーションの起動 ---
    // 先ほど作成した App.cpp 内の StartApp を呼び出します。
    // 引数にはウィンドウのタイトルバーに表示する文字列を渡します。
    StartApp(TEXT("DirectX12 Engine - 2560x1600 Migration"));

    // --- [4] COMライブラリの解放 ---
    CoUninitialize();

    return 0;
}