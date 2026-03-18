#pragma once
#include "ComPtr.h"
#include <d3dx12.h>
#include <vector>

class ConstantBuffer;
class Texture2D;

class DescriptorHandle
{
public:
	D3D12_CPU_DESCRIPTOR_HANDLE HandleCPU;
	D3D12_GPU_DESCRIPTOR_HANDLE HandleGPU;
};

class DescriptorHeap
{
public:
	DescriptorHeap();
	~DescriptorHeap();
	ID3D12DescriptorHeap* GetHeap();
	DescriptorHandle* Register(Texture2D* texture);
	DescriptorHandle* RegisterResource(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
	DescriptorHandle* CreateUAV(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc);

private:
	bool m_IsValid = false;
	UINT m_IncrementSize = 0;
	ComPtr<ID3D12DescriptorHeap> m_pHeap = nullptr;
	std::vector<DescriptorHandle*> m_pHandles;
};
