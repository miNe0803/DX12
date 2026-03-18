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
	enum { FRAME_BUFFER_COUNT = 2 };
	static constexpr DXGI_FORMAT kDepthStencilFormat = DXGI_FORMAT_D32_FLOAT;

	bool Init(HWND hwnd, UINT windowWidth, UINT windowHeight);

	void BeginRender();
	void EndRender();

	ID3D12Device6* Device();
	ID3D12GraphicsCommandList* CommandList();
	ID3D12CommandAllocator* Allocator(UINT index);
	ID3D12CommandQueue* Queue();
	UINT CurrentBackBufferIndex();

	// Close, execute, wait (e.g. IBL generation on init).
	void ExecuteAndWait();

	// Block until GPU queue is idle (resource teardown, after external queue use).
	void WaitForGpuIdle();

	// HDR color, RTVs, back buffer
	ID3D12Resource* GetHdrColorResource();
	ID3D12Resource* GetBackBufferResource();
	D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE GetHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle();
	UINT GetFrameBufferWidth() const { return m_FrameBufferWidth; }
	UINT GetFrameBufferHeight() const { return m_FrameBufferHeight; }

private:
	bool CreateDevice();
	bool CreateCommandQueue();
	bool CreateSwapChain();
	bool CreateCommandList();
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

	ComPtr<ID3D12Device6> m_pDevice = nullptr;
	ComPtr<ID3D12CommandQueue> m_pQueue = nullptr;
	ComPtr<IDXGISwapChain3> m_pSwapChain = nullptr;
	ComPtr<ID3D12CommandAllocator> m_pAllocator[FRAME_BUFFER_COUNT] = { nullptr };
	ComPtr<ID3D12GraphicsCommandList> m_pCommandList = nullptr;

	HANDLE m_fenceEvent = nullptr;
	ComPtr<ID3D12Fence> m_pFence = nullptr;
	UINT64 m_fenceValue[FRAME_BUFFER_COUNT] = {};
	// Last fence signaled after main command list Execute (allocator 0 only for rendering).
	UINT64 m_mainGraphicsFenceValue = 0;

	D3D12_VIEWPORT m_Viewport{};
	D3D12_RECT m_Scissor{};

	UINT m_RtvDescriptorSize = 0;
	ComPtr<ID3D12DescriptorHeap> m_pRtvHeap = nullptr;
	ComPtr<ID3D12Resource> m_pRenderTargets[FRAME_BUFFER_COUNT] = { nullptr };
	ComPtr<ID3D12Resource> m_pHdrColor = nullptr;

	UINT m_DsvDescriptorSize = 0;
	ComPtr<ID3D12DescriptorHeap> m_pDsvHeap = nullptr;
	ComPtr<ID3D12Resource> m_pDepthStencilBuffer = nullptr;

	ID3D12Resource* m_currentRenderTarget = nullptr;
};

extern Engine* g_Engine;