#pragma once
#include "framework.h"



// メインループを開始する外部インターフェース
void StartApp(const TCHAR* appName);

class App {
public:
    App();
    ~App();

    // アプリケーションの実行（初期化からループ終了まで）
    void Run(const TCHAR* appName);

private:
    bool InitWindow(const TCHAR* appName); // ウィンドウ生成
    void MainLoop();                       // PeekMessageによるゲームループ
    void Terminate();                      // メモリ解放

    // Windowsメッセージプロシージャ（静的メンバ）
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    HINSTANCE m_hInst = nullptr;
    HWND      m_hWnd = nullptr;
};