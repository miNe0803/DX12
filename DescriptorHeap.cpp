#include "DescriptorHeap.h"
#include "Texture2D.h"
#include <d3dx12.h>
#include "Engine.h"

const UINT HANDLE_MAX = 512;

DescriptorHeap::DescriptorHeap()
{
	m_pHandles.clear();
	m_pHandles.reserve(HANDLE_MAX);

	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.NodeMask = 0;
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = HANDLE_MAX;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	auto device = g_Engine->Device();

	auto hr = device->CreateDescriptorHeap(
		&desc,
		IID_PPV_ARGS(m_pHeap.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) {
		printf("CreateDescriptorHeap failed: 0x%08X\n", hr);
		if (hr == DXGI_ERROR_DEVICE_REMOVED) {
			printf("Reason: 0x%08X\n", device->GetDeviceRemovedReason());
		}
		m_IsValid = false; return;
	}

	m_IncrementSize = device->GetDescriptorHandleIncrementSize(desc.Type);
	m_IsValid = true;
}

ID3D12DescriptorHeap* DescriptorHeap::GetHeap()
{
	return m_pHeap.Get();
}

DescriptorHandle* DescriptorHeap::Register(Texture2D* texture)
{
	if (texture == nullptr)
		return nullptr;
	auto count = m_pHandles.size();
	if (HANDLE_MAX <= count)
	{
		return nullptr;
	}

	DescriptorHandle* pHandle = new DescriptorHandle();

	auto handleCPU = m_pHeap->GetCPUDescriptorHandleForHeapStart();
	handleCPU.ptr += m_IncrementSize * count;

	auto handleGPU = m_pHeap->GetGPUDescriptorHandleForHeapStart();
	handleGPU.ptr += m_IncrementSize * count;

	pHandle->HandleCPU = handleCPU;
	pHandle->HandleGPU = handleGPU;

	auto device = g_Engine->Device();
	auto resource = texture->Resource();
	auto desc = texture->ViewDesc();
	device->CreateShaderResourceView(resource, &desc, pHandle->HandleCPU);

	m_pHandles.push_back(pHandle);
	return pHandle;
}

DescriptorHandle* DescriptorHeap::RegisterResource(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
	if (resource == nullptr) return nullptr;
	auto count = m_pHandles.size();
	if (HANDLE_MAX <= count) return nullptr;

	DescriptorHandle* pHandle = new DescriptorHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE handleCPU = m_pHeap->GetCPUDescriptorHandleForHeapStart();
	handleCPU.ptr += m_IncrementSize * count;
	D3D12_GPU_DESCRIPTOR_HANDLE handleGPU = m_pHeap->GetGPUDescriptorHandleForHeapStart();
	handleGPU.ptr += m_IncrementSize * count;

	pHandle->HandleCPU = handleCPU;
	pHandle->HandleGPU = handleGPU;

	g_Engine->Device()->CreateShaderResourceView(resource, &srvDesc, pHandle->HandleCPU);
	m_pHandles.push_back(pHandle);
	return pHandle;
}

DescriptorHandle* DescriptorHeap::CreateUAV(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc)
{
	if (resource == nullptr) return nullptr;
	auto count = m_pHandles.size();
	if (HANDLE_MAX <= count) return nullptr;

	DescriptorHandle* pHandle = new DescriptorHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE handleCPU = m_pHeap->GetCPUDescriptorHandleForHeapStart();
	handleCPU.ptr += m_IncrementSize * count;
	D3D12_GPU_DESCRIPTOR_HANDLE handleGPU = m_pHeap->GetGPUDescriptorHandleForHeapStart();
	handleGPU.ptr += m_IncrementSize * count;

	pHandle->HandleCPU = handleCPU;
	pHandle->HandleGPU = handleGPU;

	g_Engine->Device()->CreateUnorderedAccessView(resource, nullptr, &uavDesc, pHandle->HandleCPU);
	m_pHandles.push_back(pHandle);
	return pHandle;
}
