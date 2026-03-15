#include "App.h"
#include "Engine.h"
#include "Scene.h"
#include "keyboard.h"
#include <tchar.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern Engine* g_Engine;
extern Scene* g_Scene;

void StartApp(const TCHAR* appName) {
    App app;
    app.Run(appName);
}

App::App() : m_hInst(nullptr), m_hWnd(nullptr) {}

App::~App() {
    Terminate();
}

void App::Run(const TCHAR* appName) {
    if (!InitWindow(appName)) {
        return;
    }

    g_Engine = new Engine();
    if (!g_Engine->Init(m_hWnd, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        printf("Engine init failed. Window only.\n");
        delete g_Engine;
        g_Engine = nullptr;
    }

    Keyboard_Initialize();

    if (g_Engine) {
        g_Scene = new Scene();
        if (!g_Scene->Init()) {
            printf("Scene init failed. Window only.\n");
            delete g_Scene;
            g_Scene = nullptr;
        }
    } else {
        g_Scene = nullptr;
    }

    MainLoop();
}

bool App::InitWindow(const TCHAR* appName) {
    m_hInst = GetModuleHandle(nullptr);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = m_hInst;
    wc.hIcon = LoadIcon(m_hInst, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = appName;

    if (!RegisterClassEx(&wc)) {
        return false;
    }

    RECT rect = { 0, 0, static_cast<LONG>(WINDOW_WIDTH), static_cast<LONG>(WINDOW_HEIGHT) };
    auto style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);

    m_hWnd = CreateWindowEx(
        0, appName, appName, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, m_hInst, nullptr
    );

    if (!m_hWnd) {
        return false;
    }

    ShowWindow(m_hWnd, SW_SHOWNORMAL);
    UpdateWindow(m_hWnd);
    SetFocus(m_hWnd);

    return true;
}

void App::MainLoop() {
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            Keyboard_Update();

            if (g_Scene) {
                g_Scene->Update();
            }

            if (g_Engine && g_Scene) {
                g_Engine->BeginRender();
                g_Scene->Draw();
                g_Engine->EndRender();
            }
        }
    }
}

void App::Terminate() {
    if (g_Scene) {
        delete g_Scene;
        g_Scene = nullptr;
    }
    if (g_Engine) {
        delete g_Engine;
        g_Engine = nullptr;
    }
}

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    Keyboard_ProcessMessage(msg, wp, lp);

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}
