#pragma once
#include "ComPtr.h"
#include <d3dx12.h>
#include <vector>
#include <cstdint>

class ConstantBuffer;
class Texture2D;

class DescriptorHandle
{
public:
	D3D12_CPU_DESCRIPTOR_HANDLE HandleCPU;
	D3D12_GPU_DESCRIPTOR_HANDLE HandleGPU;
	/// Bindless heap index (usable with ResourceDescriptorHeap[heapIndex] in SM6.6).
	uint32_t HeapIndex = 0;
};

class DescriptorHeap
{
public:
	static constexpr uint32_t kMaxDescriptors = 65536u;

	DescriptorHeap();
	~DescriptorHeap();
	ID3D12DescriptorHeap* GetHeap();

	// --- Legacy API (returns DescriptorHandle*, backward compatible) ---
	DescriptorHandle* Register(Texture2D* texture);
	DescriptorHandle* RegisterResource(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
	DescriptorHandle* CreateUAV(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc);

	// --- Bindless API (returns heap index for ResourceDescriptorHeap[]) ---
	/// Allocate the next free descriptor slot. Returns heap index, or UINT32_MAX on failure.
	uint32_t AllocateIndex();
	/// Place an SRV at a specific heap index (obtained from AllocateIndex).
	void CreateSRVAt(uint32_t index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
	/// Place a UAV at a specific heap index.
	void CreateUAVAt(uint32_t index, ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc);
	/// Current number of allocated descriptors.
	uint32_t AllocatedCount() const { return m_nextFreeIndex; }

private:
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleAt(uint32_t index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleAt(uint32_t index) const;

	bool m_IsValid = false;
	UINT m_IncrementSize = 0;
	uint32_t m_nextFreeIndex = 0;
	ComPtr<ID3D12DescriptorHeap> m_pHeap = nullptr;
	std::vector<DescriptorHandle*> m_pHandles; // legacy ownership
};
