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
    // DX12開発において非常に重要です。リソースの状態遷移ミスや
    // メモリリークなどのエラーを「出力」ウィンドウに詳しく報告してくれます。
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
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