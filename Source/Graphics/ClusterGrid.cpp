#include "ClusterGrid.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool ClusterGrid::Init(ID3D12Device* device, DescriptorHeap* heap, uint32_t screenW, uint32_t screenH)
{
	if (!device || !heap || screenW == 0 || screenH == 0) return false;

	m_screenW = screenW;
	m_screenH = screenH;
	m_tileCountX = (screenW + kTileSize - 1) / kTileSize;
	m_tileCountY = (screenH + kTileSize - 1) / kTileSize;
	m_totalClusters = m_tileCountX * m_tileCountY * kDepthSlices;

	if (!CreateBuffers(device, heap)) return false;
	if (!CreatePipeline(device)) return false;

	m_valid = true;
	printf("ClusterGrid::Init: %ux%u tiles, %u slices, %u clusters total\n",
		m_tileCountX, m_tileCountY, kDepthSlices, m_totalClusters);
	return true;
}

void ClusterGrid::Shutdown()
{
	if (m_cbUpload && m_cbMapped) { m_cbUpload->Unmap(0, nullptr); m_cbMapped = nullptr; }
	m_lightBuffer.Reset(); m_lightUpload.Reset();
	m_clusterDataBuffer.Reset(); m_lightIndexListBuffer.Reset();
	m_globalCounterBuffer.Reset(); m_counterResetUpload.Reset();
	m_rootSig.Reset(); m_pso.Reset(); m_cbUpload.Reset();
	m_valid = false;
}

bool ClusterGrid::CreateBuffers(ID3D12Device* device, DescriptorHeap* heap)
{
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto uploadProp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	// Light buffer
	{
		UINT64 sz = sizeof(GpuLight) * kMaxLights;
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sz);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_lightBuffer));
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightUpload));

		m_lightBufferSrvIdx = heap->AllocateIndex();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = kMaxLights;
		srv.Buffer.StructureByteStride = sizeof(GpuLight);
		heap->CreateSRVAt(m_lightBufferSrvIdx, m_lightBuffer.Get(), srv);
	}

	// Cluster data: per-cluster uint2 (offset, count)
	{
		UINT64 sz = m_totalClusters * sizeof(uint32_t) * 2;
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sz, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_clusterDataBuffer));

		m_clusterDataSrvIdx = heap->AllocateIndex();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = m_totalClusters;
		srv.Buffer.StructureByteStride = sizeof(uint32_t) * 2;
		heap->CreateSRVAt(m_clusterDataSrvIdx, m_clusterDataBuffer.Get(), srv);

		m_clusterDataUavIdx = heap->AllocateIndex();
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = m_totalClusters;
		uav.Buffer.StructureByteStride = sizeof(uint32_t) * 2;
		heap->CreateUAVAt(m_clusterDataUavIdx, m_clusterDataBuffer.Get(), uav);
	}

	// Light index list: worst case = totalClusters * kMaxLightsPerCluster
	const uint32_t maxIndices = m_totalClusters * kMaxLightsPerCluster;
	{
		UINT64 sz = maxIndices * sizeof(uint32_t);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sz, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_lightIndexListBuffer));

		m_lightIndexListSrvIdx = heap->AllocateIndex();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32_UINT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = maxIndices;
		heap->CreateSRVAt(m_lightIndexListSrvIdx, m_lightIndexListBuffer.Get(), srv);

		m_lightIndexListUavIdx = heap->AllocateIndex();
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_UINT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = maxIndices;
		heap->CreateUAVAt(m_lightIndexListUavIdx, m_lightIndexListBuffer.Get(), uav);
	}

	// Global counter (single uint32)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_globalCounterBuffer));

		m_globalCounterUavIdx = heap->AllocateIndex();
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		heap->CreateUAVAt(m_globalCounterUavIdx, m_globalCounterBuffer.Get(), uav);

		auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_counterResetUpload));
		void* mapped = nullptr;
		D3D12_RANGE r{0,0};
		m_counterResetUpload->Map(0, &r, &mapped);
		memset(mapped, 0, sizeof(uint32_t));
		m_counterResetUpload->Unmap(0, nullptr);
	}

	// Constants CB
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cbUpload));
		D3D12_RANGE r{0,0};
		m_cbUpload->Map(0, &r, &m_cbMapped);
	}

	return true;
}

bool ClusterGrid::CreatePipeline(ID3D12Device* device)
{
	// Root signature: CBV b0, 4 UAVs (cluster data, light index list, global counter, —), SRV light buffer
	CD3DX12_ROOT_PARAMETER1 params[3]{};
	params[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);

	// Descriptor table: 3 UAVs (u0=clusterData, u1=lightIndexList, u2=globalCounter)
	CD3DX12_DESCRIPTOR_RANGE1 uavRanges[3]{};
	uavRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0
	uavRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1); // u1
	uavRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2); // u2
	params[1].InitAsDescriptorTable(3, uavRanges);

	// SRV: t0 = light buffer
	CD3DX12_DESCRIPTOR_RANGE1 srvRange{};
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	params[2].InitAsDescriptorTable(1, &srvRange);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init_1_1(3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> rsBlob, rsErr;
	HRESULT hr = D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
		rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr)) { printf("ClusterGrid RS serialize failed\n"); return false; }

	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
		IID_PPV_ARGS(m_rootSig.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	hr = D3DReadFileToBlob(L"ClusterLightAssign_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"Shaders\\ClusterLightAssign_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
	{
		printf("ClusterGrid: ClusterLightAssign_CS.cso not found (will be compiled later)\n");
		// Pipeline can be created later when shader is available
		return true; // non-fatal: system works without the PSO
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = m_rootSig.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_pso.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) { printf("ClusterGrid PSO create failed\n"); return false; }

	return true;
}

void ClusterGrid::UpdateLights(ID3D12GraphicsCommandList* cmd, const GpuLight* lights, uint32_t count)
{
	m_activeLightCount = (std::min)(count, kMaxLights);
	if (m_activeLightCount == 0) return;

	UINT64 sz = m_activeLightCount * sizeof(GpuLight);
	void* mapped = nullptr;
	D3D12_RANGE r{0,0};
	if (SUCCEEDED(m_lightUpload->Map(0, &r, &mapped)))
	{
		memcpy(mapped, lights, static_cast<size_t>(sz));
		m_lightUpload->Unmap(0, nullptr);
	}

	auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_lightBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->ResourceBarrier(1, &b1);
	cmd->CopyBufferRegion(m_lightBuffer.Get(), 0, m_lightUpload.Get(), 0, sz);
	auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(m_lightBuffer.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &b2);
}

void ClusterGrid::AssignLights(ID3D12GraphicsCommandList* cmd,
	const XMMATRIX& view, const XMMATRIX& proj, float nearZ, float farZ)
{
	if (!m_valid || !m_pso || m_activeLightCount == 0) return;

	// Compute log-depth parameters
	m_logScale = static_cast<float>(kDepthSlices) / std::log2(farZ / nearZ);
	m_logBias  = -m_logScale * std::log2(nearZ);

	// Fill CB
	ClusterAssignCB cb{};
	cb.InvProj = XMMatrixTranspose(XMMatrixInverse(nullptr, proj));
	cb.View = XMMatrixTranspose(view);
	cb.nearZ = nearZ;
	cb.farZ = farZ;
	cb.logScale = m_logScale;
	cb.logBias = m_logBias;
	cb.tileCountX = m_tileCountX;
	cb.tileCountY = m_tileCountY;
	cb.depthSlices = kDepthSlices;
	cb.lightCount = m_activeLightCount;
	cb.screenW = m_screenW;
	cb.screenH = m_screenH;
	if (m_cbMapped) memcpy(m_cbMapped, &cb, sizeof(cb));

	// Reset counter and cluster data
	{
		D3D12_RESOURCE_BARRIER barriers[3];
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_globalCounterBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_clusterDataBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(m_lightIndexListBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(3, barriers);

		cmd->CopyBufferRegion(m_globalCounterBuffer.Get(), 0, m_counterResetUpload.Get(), 0, sizeof(uint32_t));
		auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_globalCounterBuffer.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b);
	}

	// TODO: set descriptor heap and dispatch when CS is compiled
	// For now, the infrastructure is ready for integration.
	// Dispatch would be: cmd->Dispatch(m_tileCountX, m_tileCountY, kDepthSlices);
}
