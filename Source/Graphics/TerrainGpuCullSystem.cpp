#include "TerrainGpuCullSystem.h"
#include "DescriptorHeap.h"
#include "Engine.h"
#include "SharedStruct.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "DebugLog.h"

#include <d3dcompiler.h>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// Scene.cpp に定義された拡張テレインマスク GPU ハンドル取得 (t9-t12 用)
extern D3D12_GPU_DESCRIPTOR_HANDLE Scene_GetTerrainExtraMaskGpu();

namespace
{
	struct TerrainChunkGpu
	{
		XMFLOAT4 aabbMin{};
		XMFLOAT4 aabbMax{};
		UINT     startIndex[kTerrainLodCount]{};
		UINT     indexCount[kTerrainLodCount]{};
		UINT     pad[2]{};
	};
	static_assert(sizeof(TerrainChunkGpu) == 56, "TerrainChunkGpu stride");

	struct TerrainDrawPayloadGpu
	{
		UINT  chunkId = 0;
		UINT  lod = 0;
		float morph = 0.0f;
		float pad = 0.0f;
	};
	static_assert(sizeof(TerrainDrawPayloadGpu) == 16, "TerrainDrawPayloadGpu stride");

	struct alignas(256) TerrainFrustumCullCB
	{
		XMFLOAT4 Planes[6]{};
		XMFLOAT4 CameraPos{};
		XMFLOAT4 CullParams{};
		XMFLOAT4X4 ViewProj{};
		XMFLOAT4 HiZParams{};
		XMFLOAT4 HiZTuning{};
		XMFLOAT4 _padTo256[2]{};
	};
	static_assert(sizeof(TerrainFrustumCullCB) == 256, "TerrainFrustumCull CB");


	bool ExtractFrustumPlanes(FXMMATRIX vp, XMFLOAT4 planes[6])
	{
		// VP 行列が特異（カメラ真上/真下等）なら抽出を中止
		XMVECTOR det;
		(void)XMMatrixInverse(&det, vp);
		if (XMVectorGetX(XMVectorAbs(det)) < 1e-10f)
			return false;

		const XMVECTOR r0 = vp.r[0];
		const XMVECTOR r1 = vp.r[1];
		const XMVECTOR r2 = vp.r[2];
		const XMVECTOR r3 = vp.r[3];
		XMStoreFloat4(&planes[0], XMVectorAdd(r3, r0));
		XMStoreFloat4(&planes[1], XMVectorSubtract(r3, r0));
		XMStoreFloat4(&planes[2], XMVectorAdd(r3, r1));
		XMStoreFloat4(&planes[3], XMVectorSubtract(r3, r1));
		XMStoreFloat4(&planes[4], XMVectorAdd(r3, r2));
		XMStoreFloat4(&planes[5], XMVectorSubtract(r3, r2));
		return true;
	}

	bool AabbOutsidePlaneCpu(const ModelBounds& b, const XMFLOAT4& pl)
	{
		for (int c = 0; c < 8; ++c)
		{
			const float x = (c & 1) ? b.Max.x : b.Min.x;
			const float y = (c & 2) ? b.Max.y : b.Min.y;
			const float z = (c & 4) ? b.Max.z : b.Min.z;
			const float d = pl.x * x + pl.y * y + pl.z * z + pl.w;
			if (d <= 0.0f)
				return false;
		}
		return true;
	}

	bool IsAabbCulledCpu(const ModelBounds& b, const XMFLOAT4 planes[6])
	{
		for (int p = 0; p < 6; ++p)
		{
			if (AabbOutsidePlaneCpu(b, planes[p]))
				return true;
		}
		return false;
	}

	bool LoadCsBlob(ComPtr<ID3DBlob>& outBlob)
	{
		HRESULT hr = D3DReadFileToBlob(L"TerrainFrustumCull_CS.cso", outBlob.ReleaseAndGetAddressOf());
		if (FAILED(hr))
			hr = D3DReadFileToBlob(L"Shaders\\TerrainFrustumCull_CS.cso", outBlob.ReleaseAndGetAddressOf());
		return SUCCEEDED(hr) && outBlob;
	}
}

TerrainGpuCullSystem::~TerrainGpuCullSystem()
{
	Shutdown();
}

void TerrainGpuCullSystem::Shutdown()
{
	m_computeRootSig.Reset();
	m_computePso.Reset();
	m_drawIndexedSig.Reset();
	m_chunkInfoDefault.Reset();
	m_chunkUpload.Reset();
	m_indirectArgsDefault.Reset();
	m_counterDefault.Reset();
	m_counterResetUpload.Reset();
	m_cullCBUpload.Reset();
	m_drawPayloadDefault.Reset();
	m_hizFallbackResource.Reset();
	m_descriptorHeap = nullptr;
	m_srvChunk = nullptr;
	m_uavArgs = nullptr;
	m_uavCounter = nullptr;
	m_uavPayload = nullptr;
	m_hizFallbackSrv = nullptr;
	m_hizSrv = nullptr;
	m_counterClearCpuValid = false;
	m_valid = false;
	m_chunkCount = 0;
	m_counterResState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_indirectResState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_payloadResState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_hizEnabled = false;
	m_hizUserEnabled = true;
	m_hizWidth = m_hizHeight = m_hizMipCount = 1;
	m_hizNearDisableDistance = 200.0f;
	m_hizDepthBias = 0.01f;
	m_hizMaxPixelRadius = 96.0f;
	m_lod0StartDistance = 300.0f;
	m_lod1StartDistance = 900.0f;
	m_forceLod1 = false;
	m_debugLastLod0VisibleCount = 0;
	m_debugLastLod1VisibleCount = 0;
	m_debugLastGpuVisibleCount = 0;
	m_debugLastLod0IndexCount = 0;
	m_debugLastLod1IndexCount = 0;
	m_chunkBoundsCpu.clear();
	m_chunkLod0IndexCountCpu.clear();
	m_chunkLod1IndexCountCpu.clear();
	m_counterReadback.clear();
}

bool TerrainGpuCullSystem::CreatePipelines(ID3D12Device* device)
{
	ComPtr<ID3DBlob> csBlob;
	if (!LoadCsBlob(csBlob))
	{
		DebugLog("[TerrainGpuCull] TerrainFrustumCull_CS.cso not found\n");
		return false;
	}

	CD3DX12_DESCRIPTOR_RANGE hizSrvRange;
	hizSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
	CD3DX12_DESCRIPTOR_RANGE uavArgsRange;
	uavArgsRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	CD3DX12_DESCRIPTOR_RANGE uavCntRange;
	uavCntRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
	CD3DX12_DESCRIPTOR_RANGE uavPayloadRange;
	uavPayloadRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);

	CD3DX12_ROOT_PARAMETER rootParams[6] = {};
	rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
	rootParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0: chunk buffer
	rootParams[2].InitAsDescriptorTable(1, &hizSrvRange, D3D12_SHADER_VISIBILITY_ALL); // t1: Hi-Z
	rootParams[3].InitAsDescriptorTable(1, &uavArgsRange, D3D12_SHADER_VISIBILITY_ALL);
	rootParams[4].InitAsDescriptorTable(1, &uavCntRange, D3D12_SHADER_VISIBILITY_ALL);
	rootParams[5].InitAsDescriptorTable(1, &uavPayloadRange, D3D12_SHADER_VISIBILITY_ALL);

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 6;
	rsDesc.pParameters = rootParams;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsBlob, rsErr;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr))
		return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_computeRootSig.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_computeRootSig.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_computePso.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

bool TerrainGpuCullSystem::CreateIndirectCommandSignature(ID3D12Device* device)
{
	D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
	argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
	sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	sigDesc.NumArgumentDescs = 1;
	sigDesc.pArgumentDescs = &argDesc;

	const HRESULT hr = device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(m_drawIndexedSig.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

bool TerrainGpuCullSystem::Init(ID3D12Device* device, DescriptorHeap* descriptorHeap, const std::vector<TerrainChunkDesc>& chunks)
{
	Shutdown();
	if (!device || !descriptorHeap || chunks.empty())
		return false;

	m_descriptorHeap = descriptorHeap;
	m_chunkCount = static_cast<uint32_t>(chunks.size());
	m_maxIndirectCommands = m_chunkCount;
	m_chunkBoundsCpu.resize(chunks.size());
	m_chunkLod0IndexCountCpu.resize(chunks.size());
	m_chunkLod1IndexCountCpu.resize(chunks.size());

	if (!CreatePipelines(device) || !CreateIndirectCommandSignature(device))
	{
		DebugLog("[TerrainGpuCull] pipeline or command signature failed\n");
		return false;
	}

	std::vector<TerrainChunkGpu> gpuChunks(chunks.size());
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		m_chunkBoundsCpu[i] = chunks[i].LocalBounds;
		m_chunkLod0IndexCountCpu[i] = chunks[i].IndexCount[0];
		m_chunkLod1IndexCountCpu[i] = chunks[i].IndexCount[1];
		gpuChunks[i].aabbMin = XMFLOAT4(chunks[i].LocalBounds.Min.x, chunks[i].LocalBounds.Min.y, chunks[i].LocalBounds.Min.z, 0.f);
		gpuChunks[i].aabbMax = XMFLOAT4(chunks[i].LocalBounds.Max.x, chunks[i].LocalBounds.Max.y, chunks[i].LocalBounds.Max.z, 0.f);
		for (UINT l = 0; l < kTerrainLodCount; ++l)
		{
			gpuChunks[i].startIndex[l] = chunks[i].StartIndex[l];
			gpuChunks[i].indexCount[l] = chunks[i].IndexCount[l];
		}
	}

	const UINT64 chunkBufBytes = sizeof(TerrainChunkGpu) * gpuChunks.size();
	const UINT64 indirectBytes = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) * m_maxIndirectCommands;
	const UINT64 payloadBytes = sizeof(TerrainDrawPayloadGpu) * m_maxIndirectCommands;

	auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	auto chunkDesc = CD3DX12_RESOURCE_DESC::Buffer(chunkBufBytes);
	auto indirectDesc = CD3DX12_RESOURCE_DESC::Buffer(indirectBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto payloadDesc = CD3DX12_RESOURCE_DESC::Buffer(payloadBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto counterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(chunkBufBytes);
	auto counterUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT));
	auto counterReadbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT));
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(TerrainFrustumCullCB));

	HRESULT hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &chunkDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(m_chunkInfoDefault.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_chunkUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &indirectDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_indirectArgsDefault.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &payloadDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_drawPayloadDefault.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &counterDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_counterDefault.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &counterUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_counterResetUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	{
		auto heapReadback = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
		m_counterReadback.resize(Engine::FRAME_BUFFER_COUNT);
		for (int i = 0; i < Engine::FRAME_BUFFER_COUNT; ++i)
		{
			hr = device->CreateCommittedResource(
				&heapReadback, D3D12_HEAP_FLAG_NONE, &counterReadbackDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				IID_PPV_ARGS(m_counterReadback[i].ReleaseAndGetAddressOf()));
			if (FAILED(hr))
				return false;
		}
	}
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_cullCBUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	{
		auto hizFallbackDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 1);
		hr = device->CreateCommittedResource(
			&heapDefault, D3D12_HEAP_FLAG_NONE, &hizFallbackDesc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
			IID_PPV_ARGS(m_hizFallbackResource.ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
		D3D12_SHADER_RESOURCE_VIEW_DESC hizSrvDesc = {};
		hizSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		hizSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		hizSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		hizSrvDesc.Texture2D.MostDetailedMip = 0;
		hizSrvDesc.Texture2D.MipLevels = 1;
		m_hizFallbackSrv = descriptorHeap->RegisterResource(m_hizFallbackResource.Get(), hizSrvDesc);
		m_hizSrv = m_hizFallbackSrv;
		if (!m_hizSrv)
			return false;
	}

	{
		void* map = nullptr;
		hr = m_chunkUpload->Map(0, nullptr, &map);
		if (FAILED(hr) || !map)
			return false;
		memcpy(map, gpuChunks.data(), static_cast<size_t>(chunkBufBytes));
		m_chunkUpload->Unmap(0, nullptr);
	}
	{
		UINT z = 0;
		void* map = nullptr;
		hr = m_counterResetUpload->Map(0, nullptr, &map);
		if (FAILED(hr) || !map)
			return false;
		memcpy(map, &z, sizeof(z));
		m_counterResetUpload->Unmap(0, nullptr);
	}

	if (!g_Engine)
		return false;
	ID3D12GraphicsCommandList* cmd = g_Engine->MainGraphicsCmdList();
	ID3D12CommandAllocator* alloc = g_Engine->Allocator(0);
	alloc->Reset();
	cmd->Reset(alloc, nullptr);

	cmd->CopyResource(m_chunkInfoDefault.Get(), m_chunkUpload.Get());

	const CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
		m_chunkInfoDefault.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &toSrv);

	// ExecuteAndWait() が MainGraphicsCmdList を Close するので、ここでは Close しない
	g_Engine->ExecuteAndWait();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = m_chunkCount;
	srvDesc.Buffer.StructureByteStride = sizeof(TerrainChunkGpu);

	m_srvChunk = descriptorHeap->RegisterResource(m_chunkInfoDefault.Get(), srvDesc);
	if (!m_srvChunk)
		return false;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavArgsDesc = {};
	uavArgsDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavArgsDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavArgsDesc.Buffer.FirstElement = 0;
	uavArgsDesc.Buffer.NumElements = m_maxIndirectCommands;
	uavArgsDesc.Buffer.StructureByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	uavArgsDesc.Buffer.CounterOffsetInBytes = 0;
	uavArgsDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

	m_uavArgs = descriptorHeap->CreateUAV(m_indirectArgsDefault.Get(), uavArgsDesc);
	if (!m_uavArgs)
		return false;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavCntDesc = {};
	uavCntDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavCntDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavCntDesc.Buffer.FirstElement = 0;
	uavCntDesc.Buffer.NumElements = 1;
	uavCntDesc.Buffer.StructureByteStride = sizeof(UINT);
	uavCntDesc.Buffer.CounterOffsetInBytes = 0;
	uavCntDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

	m_uavCounter = descriptorHeap->CreateUAV(m_counterDefault.Get(), uavCntDesc);
	if (!m_uavCounter)
		return false;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavPayloadDesc = {};
	uavPayloadDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavPayloadDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavPayloadDesc.Buffer.FirstElement = 0;
	uavPayloadDesc.Buffer.NumElements = m_maxIndirectCommands;
	uavPayloadDesc.Buffer.StructureByteStride = sizeof(TerrainDrawPayloadGpu);
	uavPayloadDesc.Buffer.CounterOffsetInBytes = 0;
	uavPayloadDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	m_uavPayload = descriptorHeap->CreateUAV(m_drawPayloadDefault.Get(), uavPayloadDesc);
	if (!m_uavPayload)
		return false;

	m_counterResState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_indirectResState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_payloadResState = static_cast<D3D12_RESOURCE_STATES>(0);

	m_valid = true;
	return true;
}

void TerrainGpuCullSystem::DispatchFrustumCull(ID3D12GraphicsCommandList* cmd, const SceneConstants* scene)
{
	if (!m_valid || !cmd || !scene || m_chunkCount == 0 || !m_srvChunk || !m_hizSrv || !m_uavArgs || !m_uavCounter || !m_uavPayload)
		return;

	// SceneConstants はシェーダ用に転置済みなので、CPU 側カリング計算では元の行列へ戻す。
	const XMMATRIX view = XMMatrixTranspose(scene->View);
	const XMMATRIX proj = XMMatrixTranspose(scene->Proj);
	const XMMATRIX vp = XMMatrixMultiply(view, proj);
	const XMMATRIX invView = XMMatrixInverse(nullptr, view);
	TerrainFrustumCullCB cb{};
	if (!ExtractFrustumPlanes(vp, cb.Planes))
		return; // 特異 VP — カリングをスキップ（前フレームを維持）
	XMStoreFloat4(&cb.CameraPos, invView.r[3]);
	cb.CullParams = XMFLOAT4(
		static_cast<float>(m_chunkCount),
		m_lod0StartDistance,
		(m_lod1StartDistance < m_lod0StartDistance + 1.0f) ? (m_lod0StartDistance + 1.0f) : m_lod1StartDistance,
		m_forceLod1 ? 1.0f : 0.0f);
	XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(vp));
	cb.HiZParams = XMFLOAT4(
		(m_hizEnabled && m_hizSrv) ? 1.0f : 0.0f,
		static_cast<float>(m_hizWidth),
		static_cast<float>(m_hizHeight),
		static_cast<float>(m_hizMipCount));
	cb.HiZTuning = XMFLOAT4(
		m_hizNearDisableDistance,
		m_hizDepthBias,
		m_hizMaxPixelRadius,
		0.0f);

	if (m_enableCpuDebugEstimation)
	{
		uint32_t lod0Visible = 0;
		uint32_t lod1Visible = 0;
		uint32_t lod0Indices = 0;
		uint32_t lod1Indices = 0;
		const XMFLOAT3 cam = XMFLOAT3(cb.CameraPos.x, cb.CameraPos.y, cb.CameraPos.z);
		const float lodSplit = (m_lod1StartDistance < m_lod0StartDistance + 1.0f) ? (m_lod0StartDistance + 1.0f) : m_lod1StartDistance;
		for (size_t i = 0; i < m_chunkBoundsCpu.size(); ++i)
		{
			const ModelBounds& b = m_chunkBoundsCpu[i];
			if (IsAabbCulledCpu(b, cb.Planes))
				continue;
			const float cx = 0.5f * (b.Min.x + b.Max.x);
			const float cy = 0.5f * (b.Min.y + b.Max.y);
			const float cz = 0.5f * (b.Min.z + b.Max.z);
			const float dx = cx - cam.x;
			const float dy = cy - cam.y;
			const float dz = cz - cam.z;
			const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
			const float lodF = (dist - m_lod0StartDistance) / (lodSplit - m_lod0StartDistance);
			const bool lod1 = m_forceLod1 || (lodF >= 1.0f);
			if (lod1)
			{
				++lod1Visible;
				lod1Indices += m_chunkLod1IndexCountCpu[i];
			}
			else
			{
				++lod0Visible;
				lod0Indices += m_chunkLod0IndexCountCpu[i];
			}
		}
		m_debugLastLod0VisibleCount = lod0Visible;
		m_debugLastLod1VisibleCount = lod1Visible;
		m_debugLastLod0IndexCount = lod0Indices;
		m_debugLastLod1IndexCount = lod1Indices;
	}

	void* map = nullptr;
	const HRESULT hr = m_cullCBUpload->Map(0, nullptr, &map);
	if (FAILED(hr) || !map)
		return;
	memcpy(map, &cb, sizeof(cb));
	m_cullCBUpload->Unmap(0, nullptr);

	{
		CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			m_counterDefault.Get(), m_counterResState, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->ResourceBarrier(1, &b);
		m_counterResState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	cmd->CopyBufferRegion(m_counterDefault.Get(), 0, m_counterResetUpload.Get(), 0, sizeof(UINT));
	{
		CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			m_counterDefault.Get(), m_counterResState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b);
		m_counterResState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	if (m_indirectResState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			m_indirectArgsDefault.Get(), m_indirectResState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b);
		m_indirectResState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	if (m_payloadResState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			m_drawPayloadDefault.Get(), m_payloadResState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b);
		m_payloadResState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	cmd->SetPipelineState(m_computePso.Get());
	cmd->SetComputeRootSignature(m_computeRootSig.Get());

	ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap->GetHeap() };
	cmd->SetDescriptorHeaps(1, heaps);

	cmd->SetComputeRootConstantBufferView(0, m_cullCBUpload->GetGPUVirtualAddress());
	cmd->SetComputeRootShaderResourceView(1, m_chunkInfoDefault->GetGPUVirtualAddress());
	cmd->SetComputeRootDescriptorTable(2, m_hizSrv->HandleGPU);
	cmd->SetComputeRootDescriptorTable(3, m_uavArgs->HandleGPU);
	cmd->SetComputeRootDescriptorTable(4, m_uavCounter->HandleGPU);
	cmd->SetComputeRootDescriptorTable(5, m_uavPayload->HandleGPU);

	const UINT groups = (m_chunkCount + 63u) / 64u;
	cmd->Dispatch(groups, 1, 1);

	CD3DX12_RESOURCE_BARRIER uav0 = CD3DX12_RESOURCE_BARRIER::UAV(m_indirectArgsDefault.Get());
	CD3DX12_RESOURCE_BARRIER uav1 = CD3DX12_RESOURCE_BARRIER::UAV(m_counterDefault.Get());
	CD3DX12_RESOURCE_BARRIER uav2 = CD3DX12_RESOURCE_BARRIER::UAV(m_drawPayloadDefault.Get());
	D3D12_RESOURCE_BARRIER uavBar[3] = { uav0, uav1, uav2 };
	cmd->ResourceBarrier(3, uavBar);
}

void TerrainGpuCullSystem::DrawIndirect(
	ID3D12GraphicsCommandList* cmd,
	RootSignature* terrainRootSig,
	PipelineState* terrainPso,
	D3D12_GPU_VIRTUAL_ADDRESS perDrawTransformGpu,
	D3D12_GPU_VIRTUAL_ADDRESS terrainMaterialGpu,
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskTable,
	D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
	VertexBuffer* vb,
	IndexBuffer* ib,
	D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSrvGpu,
	D3D12_GPU_VIRTUAL_ADDRESS shadowCBGpu)
{
	if (!m_valid || !cmd || !terrainRootSig || !terrainPso || !terrainRootSig->IsValid() || !terrainPso->IsValid()
		|| !vb || !ib || !m_drawIndexedSig)
		return;

	if (g_Engine)
	{
		// 前フレームの Copy は lastSubmitted と同じスロット。Present 後に進んだ Index ではなく直前に確定したバッファを見る。
		const UINT idxNow = g_Engine->CurrentBackBufferIndex();
		const UINT prevIdx = (idxNow + Engine::FRAME_BUFFER_COUNT - 1u) % Engine::FRAME_BUFFER_COUNT;
		if (prevIdx < m_counterReadback.size() && m_counterReadback[prevIdx])
		{
			void* p = nullptr;
			if (SUCCEEDED(m_counterReadback[prevIdx]->Map(0, nullptr, &p)) && p)
			{
				const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
				m_debugLastGpuVisibleCount = (v > m_maxIndirectCommands) ? m_maxIndirectCommands : v;
				m_counterReadback[prevIdx]->Unmap(0, nullptr);
			}
		}
	}

	{
		CD3DX12_RESOURCE_BARRIER b0 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_indirectArgsDefault.Get(), m_indirectResState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		CD3DX12_RESOURCE_BARRIER b1 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_counterDefault.Get(), m_counterResState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		CD3DX12_RESOURCE_BARRIER b2 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_drawPayloadDefault.Get(), m_payloadResState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		D3D12_RESOURCE_BARRIER bb[3] = { b0, b1, b2 };
		cmd->ResourceBarrier(3, bb);
		m_indirectResState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		m_counterResState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		m_payloadResState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}

	cmd->SetPipelineState(terrainPso->Get());
	cmd->SetGraphicsRootSignature(terrainRootSig->Get());
	cmd->SetGraphicsRootConstantBufferView(0, perDrawTransformGpu);
	cmd->SetGraphicsRootConstantBufferView(1, terrainMaterialGpu);
	cmd->SetGraphicsRootDescriptorTable(2, terrainMaskTable);
	if (iblTable.ptr != 0)
		cmd->SetGraphicsRootDescriptorTable(3, iblTable);
	cmd->SetGraphicsRootShaderResourceView(4, m_drawPayloadDefault->GetGPUVirtualAddress());
	if (shadowMapSrvGpu.ptr != 0)
		cmd->SetGraphicsRootDescriptorTable(5, shadowMapSrvGpu);
	if (shadowCBGpu != 0)
		cmd->SetGraphicsRootConstantBufferView(6, shadowCBGpu);
	// 拡張テレインマスク (t9-t12) — Color パスのみ。Depth Prepass は使用しない (PS 無し)。
	{
		D3D12_GPU_DESCRIPTOR_HANDLE extraMaskGpu = ::Scene_GetTerrainExtraMaskGpu();
		if (extraMaskGpu.ptr != 0)
			cmd->SetGraphicsRootDescriptorTable(7, extraMaskGpu);
	}

	D3D12_VERTEX_BUFFER_VIEW vbView = vb->View();
	D3D12_INDEX_BUFFER_VIEW ibView = ib->View();
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 1, &vbView);
	cmd->IASetIndexBuffer(&ibView);

	cmd->ExecuteIndirect(
		m_drawIndexedSig.Get(),
		m_maxIndirectCommands,
		m_indirectArgsDefault.Get(),
		0,
		m_counterDefault.Get(),
		0);

	if (g_Engine)
	{
		const UINT idx = g_Engine->CurrentBackBufferIndex();
		if (idx < m_counterReadback.size() && m_counterReadback[idx])
		{
			CD3DX12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
				m_counterDefault.Get(), m_counterResState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			cmd->ResourceBarrier(1, &toCopy);
			m_counterResState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			cmd->CopyBufferRegion(m_counterReadback[idx].Get(), 0, m_counterDefault.Get(), 0, sizeof(uint32_t));
			CD3DX12_RESOURCE_BARRIER backToIndirect = CD3DX12_RESOURCE_BARRIER::Transition(
				m_counterDefault.Get(), m_counterResState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			cmd->ResourceBarrier(1, &backToIndirect);
			m_counterResState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		}
	}

	{
		CD3DX12_RESOURCE_BARRIER a0 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_indirectArgsDefault.Get(), m_indirectResState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		CD3DX12_RESOURCE_BARRIER a1 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_counterDefault.Get(), m_counterResState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		CD3DX12_RESOURCE_BARRIER a2 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_drawPayloadDefault.Get(), m_payloadResState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		D3D12_RESOURCE_BARRIER ab[3] = { a0, a1, a2 };
		cmd->ResourceBarrier(3, ab);
		m_indirectResState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		m_counterResState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		m_payloadResState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
}

D3D12_GPU_VIRTUAL_ADDRESS TerrainGpuCullSystem::DrawPayloadBufferGpuAddress() const
{
	return m_drawPayloadDefault ? m_drawPayloadDefault->GetGPUVirtualAddress() : 0;
}

void TerrainGpuCullSystem::SetHiZResources(DescriptorHandle* hizSrv, uint32_t hizWidth, uint32_t hizHeight, uint32_t hizMipCount, bool enabled)
{
	m_hizSrv = hizSrv ? hizSrv : m_hizFallbackSrv;
	m_hizWidth = (hizWidth == 0u) ? 1u : hizWidth;
	m_hizHeight = (hizHeight == 0u) ? 1u : hizHeight;
	m_hizMipCount = (hizMipCount == 0u) ? 1u : hizMipCount;
	m_hizEnabled = enabled && m_hizUserEnabled && (hizSrv != nullptr);
}

void TerrainGpuCullSystem::SetHiZOcclusionEnabled(bool enabled)
{
	m_hizUserEnabled = enabled;
}

void TerrainGpuCullSystem::SetHiZOcclusionTuning(float nearDisableDistance, float depthBias, float maxPixelRadius)
{
	m_hizNearDisableDistance = (nearDisableDistance < 0.0f) ? 0.0f : nearDisableDistance;
	m_hizDepthBias = (depthBias < 0.0f) ? 0.0f : depthBias;
	m_hizMaxPixelRadius = (maxPixelRadius < 1.0f) ? 1.0f : maxPixelRadius;
}

void TerrainGpuCullSystem::GetHiZOcclusionTuning(float& outNearDisableDistance, float& outDepthBias, float& outMaxPixelRadius) const
{
	outNearDisableDistance = m_hizNearDisableDistance;
	outDepthBias = m_hizDepthBias;
	outMaxPixelRadius = m_hizMaxPixelRadius;
}

void TerrainGpuCullSystem::SetLodDistanceTuning(float lod0StartDistance, float lod1StartDistance)
{
	m_lod0StartDistance = (lod0StartDistance < 0.0f) ? 0.0f : lod0StartDistance;
	const float minLod1 = m_lod0StartDistance + 1.0f;
	m_lod1StartDistance = (lod1StartDistance < minLod1) ? minLod1 : lod1StartDistance;
}

void TerrainGpuCullSystem::GetLodDistanceTuning(float& outLod0StartDistance, float& outLod1StartDistance) const
{
	outLod0StartDistance = m_lod0StartDistance;
	outLod1StartDistance = m_lod1StartDistance;
}

void TerrainGpuCullSystem::SetForceLod1(bool enabled)
{
	m_forceLod1 = enabled;
}
