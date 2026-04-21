#include "RayTracingManager.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// Align to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
static constexpr uint32_t kShaderRecordAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

bool RayTracingManager::Init(ID3D12Device5* device, DescriptorHeap* heap,
	uint32_t screenW, uint32_t screenH)
{
	if (!device || !heap) return false;

	// Check DXR support
	const auto& features = g_Engine->GetFeatureSupport();
	if (!features.raytracingSupported)
	{
		printf("RayTracingManager: DXR not supported on this device.\n");
		return false;
	}

	m_screenW = screenW;
	m_screenH = screenH;

	if (!CreateReflectionResources(device, heap)) return false;
	if (!CreateRTPipeline(device)) return false;

	// Constants CB
	auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &cbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantsCB));
	D3D12_RANGE r{0,0};
	m_constantsCB->Map(0, &r, &m_constantsMapped);

	m_valid = true;
	printf("RayTracingManager::Init: OK (%ux%u reflection, half-res)\n", m_screenW/2, m_screenH/2);
	return true;
}

void RayTracingManager::Shutdown()
{
	if (m_constantsCB && m_constantsMapped) { m_constantsCB->Unmap(0, nullptr); m_constantsMapped = nullptr; }
	m_blasList.clear();
	m_tlasBuffer.Reset(); m_tlasScratch.Reset(); m_instanceDescBuffer.Reset();
	m_reflectionTexture.Reset();
	m_rtPso.Reset(); m_rtPsoProperties.Reset();
	m_shaderTable.Reset(); m_globalRootSig.Reset(); m_constantsCB.Reset();
	m_valid = false;
}

bool RayTracingManager::CreateReflectionResources(ID3D12Device5* device, DescriptorHeap* heap)
{
	uint32_t halfW = m_screenW / 2;
	uint32_t halfH = m_screenH / 2;

	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16B16A16_FLOAT, halfW, halfH, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	auto hr = device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_reflectionTexture));
	if (FAILED(hr)) { printf("RT: reflection texture failed\n"); return false; }

	// SRV for water shader sampling
	m_reflectionSrvIdx = heap->AllocateIndex();
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	heap->CreateSRVAt(m_reflectionSrvIdx, m_reflectionTexture.Get(), srv);

	// UAV for RT output
	m_reflectionUavIdx = heap->AllocateIndex();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	heap->CreateUAVAt(m_reflectionUavIdx, m_reflectionTexture.Get(), uav);

	return true;
}

bool RayTracingManager::CreateRTPipeline(ID3D12Device5* device)
{
	// Global root signature: CBV b0, SRV t0 (TLAS), UAV u0 (output), SRV t1 (depth)
	CD3DX12_ROOT_PARAMETER1 params[4]{};
	params[0].InitAsConstantBufferView(0);
	params[1].InitAsShaderResourceView(0); // TLAS
	CD3DX12_DESCRIPTOR_RANGE1 uavRange{};
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	params[2].InitAsDescriptorTable(1, &uavRange);
	params[3].InitAsShaderResourceView(1); // depth buffer

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init_1_1(4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> rsBlob, rsErr;
	auto hr = D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
		rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
		IID_PPV_ARGS(m_globalRootSig.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	// RT PSO will be created when shaders are compiled (non-fatal if missing now)
	// The RT pipeline uses D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE
	// with RayGen, ClosestHit, Miss shaders + HitGroup

	printf("RayTracingManager: RT root signature created. PSO deferred until shaders compiled.\n");
	return true;
}

uint32_t RayTracingManager::AddBLAS(ID3D12GraphicsCommandList4* cmd,
	ID3D12Resource* vertexBuffer, uint32_t vertexCount, uint32_t vertexStride,
	ID3D12Resource* indexBuffer, uint32_t indexCount)
{
	auto* device = g_Engine->Device();

	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress = vertexBuffer->GetGPUVirtualAddress();
	geomDesc.Triangles.VertexBuffer.StrideInBytes = vertexStride;
	geomDesc.Triangles.VertexCount = vertexCount;
	geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geomDesc.Triangles.IndexBuffer = indexBuffer->GetGPUVirtualAddress();
	geomDesc.Triangles.IndexCount = indexCount;
	geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
	device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

	BLASEntry entry{};
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	auto blasDesc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ResultDataMaxSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &blasDesc,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
		IID_PPV_ARGS(&entry.blasBuffer));

	auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &scratchDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&entry.scratchBuffer));

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
	buildDesc.Inputs = inputs;
	buildDesc.DestAccelerationStructureData = entry.blasBuffer->GetGPUVirtualAddress();
	buildDesc.ScratchAccelerationStructureData = entry.scratchBuffer->GetGPUVirtualAddress();

	cmd->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(entry.blasBuffer.Get());
	cmd->ResourceBarrier(1, &uavBarrier);

	uint32_t idx = static_cast<uint32_t>(m_blasList.size());
	m_blasList.push_back(std::move(entry));
	printf("RayTracingManager: BLAS %u built (%u verts, %u indices)\n", idx, vertexCount, indexCount);
	return idx;
}

void RayTracingManager::BuildTLAS(ID3D12GraphicsCommandList4* cmd,
	const RTInstance* instances, uint32_t instanceCount)
{
	if (instanceCount == 0) return;
	auto* device = g_Engine->Device();

	// Resize instance desc buffer if needed
	if (instanceCount > m_tlasMaxInstances)
	{
		m_tlasMaxInstances = instanceCount + 1024; // over-allocate
		auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto descBuf = CD3DX12_RESOURCE_DESC::Buffer(
			static_cast<UINT64>(m_tlasMaxInstances) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &descBuf,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_instanceDescBuffer.ReleaseAndGetAddressOf()));
	}

	// Fill instance descs
	D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
	D3D12_RANGE readRange{0,0};
	m_instanceDescBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
	for (uint32_t i = 0; i < instanceCount; ++i)
	{
		auto& inst = mapped[i];
		memset(&inst, 0, sizeof(inst));
		memcpy(inst.Transform, &instances[i].transform, sizeof(float) * 12);
		inst.InstanceID = i;
		inst.InstanceMask = static_cast<UINT>(instances[i].instanceMask);
		inst.InstanceContributionToHitGroupIndex = 0;
		inst.Flags = static_cast<UINT>(instances[i].flags);
		if (instances[i].blasIndex < m_blasList.size())
			inst.AccelerationStructure = m_blasList[instances[i].blasIndex].blasBuffer->GetGPUVirtualAddress();
	}
	m_instanceDescBuffer->Unmap(0, nullptr);

	// Prebuild info
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = instanceCount;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
	device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	// Recreate TLAS buffer if needed
	if (!m_tlasBuffer || m_tlasBuffer->GetDesc().Width < prebuild.ResultDataMaxSizeInBytes)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
			IID_PPV_ARGS(m_tlasBuffer.ReleaseAndGetAddressOf()));
	}
	if (!m_tlasScratch || m_tlasScratch->GetDesc().Width < prebuild.ScratchDataSizeInBytes)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_tlasScratch.ReleaseAndGetAddressOf()));
	}

	// Build TLAS
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
	buildDesc.Inputs = inputs;
	buildDesc.Inputs.InstanceDescs = m_instanceDescBuffer->GetGPUVirtualAddress();
	buildDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
	buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();

	cmd->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_tlasBuffer.Get());
	cmd->ResourceBarrier(1, &uavBarrier);
}

void RayTracingManager::DispatchWaterReflection(ID3D12GraphicsCommandList4* cmd,
	ID3D12Resource* depthBuffer, const XMMATRIX& invViewProj,
	const XMFLOAT3& cameraPos, float waterSurfaceY)
{
	if (!m_valid || !m_rtPso || !m_tlasBuffer) return;

	uint32_t halfW = m_screenW / 2;
	uint32_t halfH = m_screenH / 2;

	// Update constants
	if (m_constantsMapped)
	{
		auto* cb = static_cast<RTConstants*>(m_constantsMapped);
		cb->InvViewProj = XMMatrixTranspose(invViewProj);
		cb->CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0);
		cb->waterSurfaceY = waterSurfaceY;
		cb->outputWidth = halfW;
		cb->outputHeight = halfH;
	}

	// Set pipeline and dispatch
	cmd->SetComputeRootSignature(m_globalRootSig.Get());
	cmd->SetPipelineState1(m_rtPso.Get());
	cmd->SetComputeRootConstantBufferView(0, m_constantsCB->GetGPUVirtualAddress());
	cmd->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());
	// UAV and depth SRV would be set via descriptor table

	D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
	dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenRecord;
	dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_shaderRecordSize;
	dispatchDesc.MissShaderTable.StartAddress = m_missRecord;
	dispatchDesc.MissShaderTable.SizeInBytes = m_shaderRecordSize;
	dispatchDesc.MissShaderTable.StrideInBytes = m_shaderRecordSize;
	dispatchDesc.HitGroupTable.StartAddress = m_hitGroupRecord;
	dispatchDesc.HitGroupTable.SizeInBytes = m_shaderRecordSize;
	dispatchDesc.HitGroupTable.StrideInBytes = m_shaderRecordSize;
	dispatchDesc.Width = halfW;
	dispatchDesc.Height = halfH;
	dispatchDesc.Depth = 1;

	cmd->DispatchRays(&dispatchDesc);

	// UAV barrier on reflection texture
	auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(m_reflectionTexture.Get());
	cmd->ResourceBarrier(1, &barrier);
}
