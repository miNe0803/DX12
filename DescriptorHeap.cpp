#include "DescriptorHeap.h"
#include "Texture2D.h"
#include <d3dx12.h>
#include "Engine.h"

DescriptorHeap::DescriptorHeap()
{
	m_pHandles.clear();
	m_pHandles.reserve(4096); // legacy handles typically < 4096

	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.NodeMask = 0;
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = kMaxDescriptors;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	auto device = g_Engine->Device();

	auto hr = device->CreateDescriptorHeap(
		&desc,
		IID_PPV_ARGS(m_pHeap.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) {
		printf("CreateDescriptorHeap(%u) failed: 0x%08X\n", kMaxDescriptors, hr);
		if (hr == DXGI_ERROR_DEVICE_REMOVED) {
			printf("Reason: 0x%08X\n", device->GetDeviceRemovedReason());
		}
		m_IsValid = false; return;
	}

	m_IncrementSize = device->GetDescriptorHandleIncrementSize(desc.Type);
	m_nextFreeIndex = 0;
	m_IsValid = true;
	printf("DescriptorHeap: created %u descriptors (bindless-ready).\n", kMaxDescriptors);
}

DescriptorHeap::~DescriptorHeap()
{
	for (DescriptorHandle* h : m_pHandles)
		delete h;
	m_pHandles.clear();
	m_pHeap.Reset();
	m_IsValid = false;
}

ID3D12DescriptorHeap* DescriptorHeap::GetHeap()
{
	return m_pHeap.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CpuHandleAt(uint32_t index) const
{
	auto h = m_pHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_IncrementSize) * index;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GpuHandleAt(uint32_t index) const
{
	auto h = m_pHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<UINT64>(m_IncrementSize) * index;
	return h;
}

// --- Bindless API ---

uint32_t DescriptorHeap::AllocateIndex()
{
	if (m_nextFreeIndex >= kMaxDescriptors)
	{
		printf("DescriptorHeap::AllocateIndex: heap full (%u).\n", kMaxDescriptors);
		return UINT32_MAX;
	}
	return m_nextFreeIndex++;
}

void DescriptorHeap::CreateSRVAt(uint32_t index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
	if (index >= kMaxDescriptors || !resource) return;
	g_Engine->Device()->CreateShaderResourceView(resource, &srvDesc, CpuHandleAt(index));
}

void DescriptorHeap::CreateUAVAt(uint32_t index, ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc)
{
	if (index >= kMaxDescriptors || !resource) return;
	g_Engine->Device()->CreateUnorderedAccessView(resource, nullptr, &uavDesc, CpuHandleAt(index));
}

// --- Legacy API (wraps bindless allocation, backward compatible) ---

DescriptorHandle* DescriptorHeap::Register(Texture2D* texture)
{
	if (texture == nullptr)
		return nullptr;
	uint32_t idx = AllocateIndex();
	if (idx == UINT32_MAX)
	{
		printf("DescriptorHeap::Register: heap full.\n");
		return nullptr;
	}

	DescriptorHandle* pHandle = new DescriptorHandle();
	pHandle->HandleCPU = CpuHandleAt(idx);
	pHandle->HandleGPU = GpuHandleAt(idx);
	pHandle->HeapIndex = idx;

	auto resource = texture->Resource();
	auto desc = texture->ViewDesc();
	g_Engine->Device()->CreateShaderResourceView(resource, &desc, pHandle->HandleCPU);

	m_pHandles.push_back(pHandle);
	return pHandle;
}

DescriptorHandle* DescriptorHeap::RegisterResource(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
	if (resource == nullptr) return nullptr;
	uint32_t idx = AllocateIndex();
	if (idx == UINT32_MAX) return nullptr;

	DescriptorHandle* pHandle = new DescriptorHandle();
	pHandle->HandleCPU = CpuHandleAt(idx);
	pHandle->HandleGPU = GpuHandleAt(idx);
	pHandle->HeapIndex = idx;

	g_Engine->Device()->CreateShaderResourceView(resource, &srvDesc, pHandle->HandleCPU);
	m_pHandles.push_back(pHandle);
	return pHandle;
}

DescriptorHandle* DescriptorHeap::CreateUAV(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc)
{
	if (resource == nullptr) return nullptr;
	uint32_t idx = AllocateIndex();
	if (idx == UINT32_MAX) return nullptr;

	DescriptorHandle* pHandle = new DescriptorHandle();
	pHandle->HandleCPU = CpuHandleAt(idx);
	pHandle->HandleGPU = GpuHandleAt(idx);
	pHandle->HeapIndex = idx;

	g_Engine->Device()->CreateUnorderedAccessView(resource, nullptr, &uavDesc, pHandle->HandleCPU);
	m_pHandles.push_back(pHandle);
	return pHandle;
}
