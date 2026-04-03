#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>

#include <DirectXTex.h>
#include <d3dx12.h>

#include "ComPtr.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

class Engine
{
public:
	static constexpr int FRAME_BUFFER_COUNT = 2;
	static constexpr int PBR_RECORD_WORKERS = 2;
	// DSV / PSO format; resource is R32_TYPELESS with this DSV view.
	static constexpr DXGI_FORMAT kDepthStencilFormat = DXGI_FORMAT_D32_FLOAT;
	static constexpr DXGI_FORMAT kDepthStencilResourceFormat = DXGI_FORMAT_R32_TYPELESS;

	bool Init(HWND hwnd, UINT windowWidth, UINT windowHeight);

	void BeginRender();
	void EndRender();

	ID3D12Device6* Device();
	// Main pass: HDR clear + terrain (avoid "CommandList" in method names; some SDKs macro it)
	ID3D12GraphicsCommandList* MainGraphicsCmdList();
	ID3D12GraphicsCommandList* PbrRecordCmdList(int workerIndex);
	ID3D12GraphicsCommandList* PostGraphicsCmdList();
	ID3D12CommandAllocator* Allocator(UINT index);
	ID3D12CommandQueue* Queue();
	UINT CurrentBackBufferIndex();
	/// 直前の EndRender で提出したフレームが描画したスワップチェーンインデックス（Present 直前の値）。GPU プロファイラの readback スロットに使用。
	UINT LastSubmittedBackBufferIndex() const { return m_lastSubmittedBackBufferIndex; }

	void ExecuteAndWait();
	void WaitForGpuIdle();

	/// PBR・地形・スカイ用 HDR（NPR 分離時はここに NPR を描かない）
	ID3D12Resource* GetHdrColorResource();
	/// NPR キャラ専用 HDR（RGBA、透明クリア。同じ MainDepth を併用）
	ID3D12Resource* GetNprHdrColorResource();
	ID3D12Resource* GetBackBufferResource();
	D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE GetHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE GetNprHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle();
	// Depth buffer as R32_FLOAT SRV (Hi-Z, etc.)
	ID3D12Resource* GetDepthStencilResource() const { return m_pDepthStencilBuffer.Get(); }
	UINT GetFrameBufferWidth() const { return m_FrameBufferWidth; }
	UINT GetFrameBufferHeight() const { return m_FrameBufferHeight; }

private:
	bool CreateDevice();
	bool CreateCommandQueue();
	bool CreateSwapChain();
	bool InitGfxCommandLists();
	bool CreateFence();
	void CreateViewPort();
	void CreateScissorRect();

	bool CreateRenderTarget();
	bool CreateDepthStencil();

private:
	HWND m_hWnd = nullptr;
	UINT m_FrameBufferWidth = 0;
	UINT m_FrameBufferHeight = 0;
	UINT m_CurrentBackBufferIndex = 0;
	UINT m_lastSubmittedBackBufferIndex = 0;

	ComPtr<ID3D12Device6> m_pDevice = nullptr;
	ComPtr<ID3D12CommandQueue> m_pQueue = nullptr;
	ComPtr<IDXGISwapChain3> m_pSwapChain = nullptr;
	ComPtr<ID3D12CommandAllocator> m_pAllocator[FRAME_BUFFER_COUNT] = { nullptr };
	ComPtr<ID3D12GraphicsCommandList> m_pMainGfxCmdList = nullptr;
	ComPtr<ID3D12CommandAllocator> m_pPbrRecordAllocator[PBR_RECORD_WORKERS][FRAME_BUFFER_COUNT] = {};
	ComPtr<ID3D12GraphicsCommandList> m_pPbrRecordGfxCmdList[PBR_RECORD_WORKERS] = { nullptr };
	ComPtr<ID3D12CommandAllocator> m_pPostAllocator[FRAME_BUFFER_COUNT] = {};
	ComPtr<ID3D12GraphicsCommandList> m_pPostGfxCmdList = nullptr;

	HANDLE m_fenceEvent = nullptr;
	ComPtr<ID3D12Fence> m_pFence = nullptr;
	UINT64 m_fenceValue[FRAME_BUFFER_COUNT] = {};
	UINT64 m_mainGraphicsFenceValue = 0;

	D3D12_VIEWPORT m_Viewport{};
	D3D12_RECT m_Scissor{};

	UINT m_RtvDescriptorSize = 0;
	ComPtr<ID3D12DescriptorHeap> m_pRtvHeap = nullptr;
	ComPtr<ID3D12Resource> m_pRenderTargets[FRAME_BUFFER_COUNT] = { nullptr };
	ComPtr<ID3D12Resource> m_pHdrColor = nullptr;
	ComPtr<ID3D12Resource> m_pNprHdrColor = nullptr;

	UINT m_DsvDescriptorSize = 0;
	ComPtr<ID3D12DescriptorHeap> m_pDsvHeap = nullptr;
	ComPtr<ID3D12Resource> m_pDepthStencilBuffer = nullptr;

	ID3D12Resource* m_currentRenderTarget = nullptr;
};

extern Engine* g_Engine;
