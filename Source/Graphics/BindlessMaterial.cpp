#include "BindlessMaterial.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <cstring>
#include <stdio.h>

bool MaterialManager::Init(DescriptorHeap* heap)
{
	if (!heap) return false;

	const UINT64 bufferSize = sizeof(GpuMaterialData) * kMaxMaterials;
	auto device = g_Engine->Device();

	// Default heap buffer (GPU-only, copy dest)
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
	auto hr = device->CreateCommittedResource(
		&defaultProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(m_defaultBuffer.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) {
		printf("MaterialManager::Init: default buffer failed 0x%08X\n", hr);
		return false;
	}

	// Upload heap buffer (CPU-writable staging)
	auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	hr = device->CreateCommittedResource(
		&uploadProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr, IID_PPV_ARGS(m_uploadBuffer.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) {
		printf("MaterialManager::Init: upload buffer failed 0x%08X\n", hr);
		return false;
	}

	m_gpuCapacity = kMaxMaterials;

	// Register as SRV in the bindless heap
	m_bufferSrvHeapIndex = heap->AllocateIndex();
	if (m_bufferSrvHeapIndex == UINT32_MAX) return false;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = kMaxMaterials;
	srvDesc.Buffer.StructureByteStride = sizeof(GpuMaterialData);
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	heap->CreateSRVAt(m_bufferSrvHeapIndex, m_defaultBuffer.Get(), srvDesc);

	m_materials.reserve(256);
	printf("MaterialManager::Init: OK (SRV heap index %u, max %u materials).\n",
		m_bufferSrvHeapIndex, kMaxMaterials);
	return true;
}

uint32_t MaterialManager::RegisterMaterial(const GpuMaterialData& mat)
{
	if (m_materials.size() >= kMaxMaterials)
	{
		printf("MaterialManager: max materials reached (%u).\n", kMaxMaterials);
		return UINT32_MAX;
	}
	uint32_t idx = static_cast<uint32_t>(m_materials.size());
	m_materials.push_back(mat);
	m_dirty = true;
	return idx;
}

void MaterialManager::UploadToGpu(ID3D12GraphicsCommandList* cmd)
{
	if (!m_dirty || m_materials.empty()) return;

	const UINT64 copySize = sizeof(GpuMaterialData) * m_materials.size();

	// Map upload buffer and copy CPU data
	void* mapped = nullptr;
	D3D12_RANGE readRange{ 0, 0 };
	if (SUCCEEDED(m_uploadBuffer->Map(0, &readRange, &mapped)))
	{
		memcpy(mapped, m_materials.data(), static_cast<size_t>(copySize));
		m_uploadBuffer->Unmap(0, nullptr);
	}

	// Copy upload → default
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		m_defaultBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->ResourceBarrier(1, &barrier);

	cmd->CopyBufferRegion(m_defaultBuffer.Get(), 0, m_uploadBuffer.Get(), 0, copySize);

	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		m_defaultBuffer.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &barrier);

	m_dirty = false;
}
