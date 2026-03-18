#pragma once

#include <dxgiformat.h>
#include <d3d12.h>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct ID3D12DescriptorHeap;

/// ImGui 専用 CBV/SRV/UAV ヒープ。ゲーム側ディスクリプタと分離。
class ImGuiManager
{
public:
	ImGuiManager() = default;
	~ImGuiManager();

	bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, void* hwnd, int numFramesInFlight, DXGI_FORMAT rtvFormat);
	void Shutdown();

	void NewFrame();
	void Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
		UINT renderWidth, UINT renderHeight);

	bool IsInitialized() const { return m_initialized; }

private:
	bool m_initialized = false;
	ID3D12Device* m_device = nullptr;
	ID3D12CommandQueue* m_queue = nullptr;
	ID3D12DescriptorHeap* m_srvHeap = nullptr;
	void* m_srvAlloc = nullptr; // SrvHeapAllocator（.cpp 内）
	int m_numFrames = 0;
	DXGI_FORMAT m_rtvFormat = DXGI_FORMAT_UNKNOWN;
};
