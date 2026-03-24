#include "App.h"
#include "Engine.h"
#include "Scene.h"
#include "keyboard.h"
#include "Editor/ImGuiManager.h"
#include "Editor/EditorUI.h"
#include "Editor/DebugUI.h"
#include "Engine/Core/AsyncModelLoader.h"
#include "Engine/Profiling/Profiler.h"
#include "Texture2D.h"
#include "Engine/ECS/Systems/TransformSystem.h"
#include <chrono>
#include <tchar.h>
#include <windows.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern Engine* g_Engine;
extern Scene* g_Scene;

static ImGuiManager* g_ImGuiManager = nullptr;
static EditorUI      g_EditorUI;

void StartApp(const TCHAR* appName) {
    App app;
    app.Run(appName);
}

App::App() : m_hInst(nullptr), m_hWnd(nullptr) {}

App::~App() {
    Terminate();
}

void App::Run(const TCHAR* appName) {
    // 重要: DPI aware はウィンドウ生成前に有効化しないと、マウス座標がスケーリングされて
    // ImGui のクリック位置が大きくズレることがある。
    ImGui_ImplWin32_EnableDpiAwareness();

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
        else {
            g_ImGuiManager = new ImGuiManager();
            if (!g_ImGuiManager->Init(
                    g_Engine->Device(),
                    g_Engine->Queue(),
                    m_hWnd,
                    Engine::FRAME_BUFFER_COUNT,
                    DXGI_FORMAT_R8G8B8A8_UNORM))
            {
                delete g_ImGuiManager;
                g_ImGuiManager = nullptr;
            }
            else
            {
                // After ImGui uploads on shared queue, advance engine fence.
                g_Engine->WaitForGpuIdle();
            }
            g_AsyncModelLoader = new AsyncModelLoader();
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

            // CPU中心のフレーム時間計測（メインループ1周）
            Profiler::BeginFrame();
            const auto cpuFrameLoopStart = std::chrono::high_resolution_clock::now();

            if (g_Scene) {
                g_Scene->Update();
            }

            if (g_ImGuiManager && g_ImGuiManager->IsInitialized()) {
                g_ImGuiManager->NewFrame();
                if (g_Scene)
                {
                    g_EditorUI.Draw(g_Scene->GetRegistry());
                    // EditorUI とは別ウィンドウ（GPU 統計・NPR ramp デバッグ等）。未呼び出しだと UI が出ない。
                    DebugUI::Draw();
                    // Update() が ImGui より先なので、NPR デバッグ Combo を即反映
                    g_Scene->SyncNprGpuTuningToMaterialCB();
                }
            }

            // Update() 内の TransformSystem のあとに ImGui で Position 等を書き換えるため、
            // 描画直前にもう一度 WorldMatrix を計算しないとモデルが動かない。
            if (g_Scene)
                TransformSystem::Update(g_Scene->GetRegistry());

            {
                const auto tPreRenderEnd = std::chrono::high_resolution_clock::now();
                Profiler::SetPreRenderCpuTimeMs(
                    std::chrono::duration<float, std::milli>(tPreRenderEnd - cpuFrameLoopStart).count());
            }
            if (g_Engine && g_Scene) {
                const auto t0 = std::chrono::high_resolution_clock::now();
                g_Engine->BeginRender();
                const auto t1 = std::chrono::high_resolution_clock::now();
                g_Scene->Draw();
                const auto t2 = std::chrono::high_resolution_clock::now();
                if (g_ImGuiManager && g_ImGuiManager->IsInitialized()) {
                    g_ImGuiManager->Render(
                        g_Engine->PostGraphicsCmdList(),
                        g_Engine->GetBackBufferRtvCpuHandle(),
                        g_Engine->GetFrameBufferWidth(),
                        g_Engine->GetFrameBufferHeight());
                }
                g_Engine->EndRender();
                const auto t3 = std::chrono::high_resolution_clock::now();
                const float beginMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
                const float drawMs = std::chrono::duration<float, std::milli>(t2 - t1).count();
                const float endMs = std::chrono::duration<float, std::milli>(t3 - t2).count();
                Profiler::SetRenderCpuBreakdown(beginMs, drawMs, endMs);
            }

            Profiler::EndFrame();
        }
    }
}

void App::Terminate() {
    if (g_AsyncModelLoader) {
        delete g_AsyncModelLoader;
        g_AsyncModelLoader = nullptr;
    }
    if (g_ImGuiManager) {
        delete g_ImGuiManager;
        g_ImGuiManager = nullptr;
    }
    if (g_Scene) {
        delete g_Scene;
        g_Scene = nullptr;
    }
    Texture2D::ReleaseAllDeviceResources();
    if (g_Engine) {
        delete g_Engine;
        g_Engine = nullptr;
    }
}

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_ImGuiManager && g_ImGuiManager->IsInitialized()) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp))
            return 1;
    }

    Keyboard_ProcessMessage(msg, wp, lp);

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}
