#include "TreeGpuCullSystem.h"

#include "DescriptorHeap.h"
#include "Engine.h"
#include "DebugLog.h"
#include "Core/GpuDebugLabels.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "SharedStruct.h"
#include "TreeVegetation.h"
#include "Core/ModelBounds.h"

#include <d3dcompiler.h>
#include <cstring>
#include <d3dx12.h>
#include <algorithm>
#include <thread>

using namespace DirectX;

namespace
{
	// ExecuteIndirect は D3D12_DRAW_INDEXED_ARGUMENTS と同一レイアウト（20 バイト）必須。
	// 以前は 32B パディングしていたが、UAV stride / CS の RWStructuredBuffer とズレて IndexCount が壊れる原因になる。
	inline constexpr UINT kTreeIndirectStrideBytes = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	inline constexpr bool kTreeDebugForceOneInstancePerBatch = false;
	inline constexpr bool kTreeDebugBypassExecuteIndirect = false;
	inline constexpr uint32_t kTreeDebugDirectInstanceCount = 128u; // used only when kTreeDebugBypassExecuteIndirect

	inline void PackTreeVisibleRootConstants(
		uint32_t out[8],
		uint32_t visibleBase,
		const XMFLOAT3& footLocal,
		float halfW,
		float height)
	{
		out[0] = visibleBase;
		std::memcpy(&out[1], &footLocal.x, sizeof(float));
		std::memcpy(&out[2], &footLocal.y, sizeof(float));
		std::memcpy(&out[3], &footLocal.z, sizeof(float));
		std::memcpy(&out[4], &halfW, sizeof(float));
		std::memcpy(&out[5], &height, sizeof(float));
		out[6] = 0u;
		out[7] = 0u;
	}

	struct TreeIndirectCmdGpu
	{
		D3D12_DRAW_INDEXED_ARGUMENTS draw;
	};
	static_assert(sizeof(TreeIndirectCmdGpu) == kTreeIndirectStrideBytes, "TreeIndirectCmdGpu must match D3D12_DRAW_INDEXED_ARGUMENTS");

	/// 視錐: Terrain と同一の V*P から CPU で6平面（球テスト用に正規化）。Hi-Z: SceneConstants と同じ View/Proj で mul(mul(p,V),P)。
	struct alignas(256) TreeCullCB
	{
		XMFLOAT4 CameraPos;
		XMFLOAT4 Params;   // x:InstanceCount, yzw unused
		XMFLOAT4 FrustumPlanes[6];
		XMMATRIX View;
		XMMATRIX Proj;
		XMFLOAT4 HiZParams; // x:enabled(0/1), y:width, z:height, w:mipCount
		XMFLOAT4 HiZTuning; // x:nearDisableDist, y:depthBias, z:maxPixelRadius, w:unused
		XMUINT4 IndexCountsTrunk;    // x:lod0, y:lod1, z:lod2, w:unused
		XMUINT4 IndexCountsLeaves;   // x:lod0, y:lod1, z:lod2, w:unused
		XMUINT4 IndexCountsBranches; // x:lod0, y:lod1, z:lod2, w:unused
		XMFLOAT4 LodParams;  // x:lod1Start (m), y:lod2Start (m), z:maxDrawDist (m, 0=unlimited), w:unused
		XMFLOAT4 _padTo512[10];
	};
	static_assert(sizeof(TreeCullCB) == 512, "TreeCullCB must be 512 bytes (D3D12 CB multiple of 256)");

	struct TreeInfoGpu
	{
		XMFLOAT4 centerRadius; // xyz center, w radius
		XMFLOAT4X4 worldRow;   // row-major world (for bounds projection)
		UINT64 instanceGpuVA;  // points to InstanceData element in default buffer
		UINT speciesIndex = 0;
		UINT _pad0 = 0;
		UINT64 _pad1 = 0;
		UINT64 _pad2 = 0;
	};
	static_assert(sizeof(TreeInfoGpu) % 16 == 0, "TreeInfoGpu align");
	static_assert(sizeof(TreeInfoGpu) == 112, "TreeInfoGpu must match TreeFrustumHiZCull_CS TreeInfo stride");

	inline constexpr int kSpeciesCount = 3;
	inline constexpr int kLodCount = 3;
	inline constexpr int kPartCount = 3;
	inline constexpr int kBatchCount = kSpeciesCount * kLodCount * kPartCount;
	inline constexpr int BatchIndex(int species, int lod, int part)
	{
		return (species * kLodCount + lod) * kPartCount + part;
	}

	// DirectXMath row-vector convention: v_clip = v_world * VP.
	// Gribb-Hartmann for row vectors extracts from COLUMNS of VP (= rows of VP^T).
	// Negate so positive half-space = outside, matching shader's d > r*|n| test.
	// DX12 uses 0-to-1 depth range so near plane = column2 (not col3+col2).
	inline void ExtractFrustumPlanesForTreeGpu(FXMMATRIX vp, XMFLOAT4 outPlanes[6])
	{
		const XMMATRIX vpT = XMMatrixTranspose(vp);
		const XMVECTOR c0 = vpT.r[0];
		const XMVECTOR c1 = vpT.r[1];
		const XMVECTOR c2 = vpT.r[2];
		const XMVECTOR c3 = vpT.r[3];
		XMVECTOR raw[6];
		raw[0] = XMVectorNegate(XMVectorAdd(c3, c0));
		raw[1] = XMVectorNegate(XMVectorSubtract(c3, c0));
		raw[2] = XMVectorNegate(XMVectorAdd(c3, c1));
		raw[3] = XMVectorNegate(XMVectorSubtract(c3, c1));
		raw[4] = XMVectorNegate(c2);
		raw[5] = XMVectorNegate(XMVectorSubtract(c3, c2));
		for (int i = 0; i < 6; ++i)
		{
			XMVECTOR len = XMVector3Length(raw[i]);
			float fLen = XMVectorGetX(len);
			if (fLen > 1e-8f)
				XMStoreFloat4(&outPlanes[i], XMVectorDivide(raw[i], len));
			else
				outPlanes[i] = XMFLOAT4(0, 0, 0, 0);
		}
	}

	bool LoadCsBlob(const wchar_t* pathPrimary, const wchar_t* pathAlt, ComPtr<ID3DBlob>& outBlob)
	{
		if (SUCCEEDED(D3DReadFileToBlob(pathPrimary, outBlob.ReleaseAndGetAddressOf())) && outBlob)
			return true;
		outBlob.Reset();
		if (pathAlt && SUCCEEDED(D3DReadFileToBlob(pathAlt, outBlob.ReleaseAndGetAddressOf())) && outBlob)
			return true;
		outBlob.Reset();
		return false;
	}

	/// 視錐テスト用の球半径（ワールド単位）。固定 2.5m だと LOD0 FBX（高さ・枝張りが大きい）で球が常に錐外扱いになり全滅する。
	float CullSphereRadiusFromMergedTreeBounds()
	{
		const ModelBounds& b = TreeVegetation::GetMergedLocalBounds();
		if (!IsValidModelBounds(b))
			return 12.f;
		const float dx = b.Max.x - b.Min.x;
		const float dy = b.Max.y - b.Min.y;
		const float dz = b.Max.z - b.Min.z;
		const float halfDiag = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
		// 足元原点付近のマージメッシュ想定。やや大きめにして偽陰性（見えるのに落とす）を避ける。
		return (std::max)(10.f, (std::min)(halfDiag * 1.25f + 2.f, 300.f));
	}

	void BuildUploadInstanceRange(
		const TreeGpuCullSystem::TreeInstanceCpu* instances,
		uint32_t begin,
		uint32_t end,
		TreeInfoGpu* outInfo,
		InstanceData* outInst,
		D3D12_GPU_VIRTUAL_ADDRESS instanceDataBaseVA)
	{
		for (uint32_t i = begin; i < end; ++i)
		{
			const XMMATRIX worldGpuT = instances[i].worldGpuT;
			const XMMATRIX worldRow = XMMatrixTranspose(worldGpuT);
			XMFLOAT4X4 wr{};
			XMStoreFloat4x4(&wr, worldRow);

			// ローカル原点のワールド位置: 行ベクトル v * W では v=(0,0,0,1) の結果が W の第4行（DirectX row-major）。
			// TreeIndirectVS の mul(float4(pos,1), worldGpuT) と XMVector4Transform(原点, worldGpuT) は転置の解釈で
			// カリング中心だけが (0,0,0) に寄り、XZ 距離が常に 0 → LOD0 固定になることがあった。
			const float radius = CullSphereRadiusFromMergedTreeBounds();
			outInfo[i].centerRadius = XMFLOAT4(wr._41, wr._42, wr._43, radius);
			outInfo[i].worldRow = wr;
			outInfo[i].instanceGpuVA = instanceDataBaseVA + static_cast<UINT64>(i) * sizeof(InstanceData);
			outInfo[i].speciesIndex = static_cast<UINT>(instances[i].speciesIndex);

			XMStoreFloat4x4(&outInst[i].World, worldGpuT);
			outInst[i].materialIndex = 0; // default PBR material
			outInst[i]._pad[0] = outInst[i]._pad[1] = outInst[i]._pad[2] = 0;
		}
	}
}

TreeGpuCullSystem::~TreeGpuCullSystem()
{
	Shutdown();
}

void TreeGpuCullSystem::Shutdown()
{
	// Safety: resources in this system can still be referenced by in-flight GPU work.
	// Ensure queue is idle before releasing D3D12 objects.
	if (g_Engine)
		g_Engine->WaitForGpuIdle();

	m_valid = false;
	m_maxInstances = 0;
	m_instanceCount = 0;
	m_descriptorHeap = nullptr;
	m_hizSrv = nullptr;
	m_hizFallbackSrv = nullptr;
	m_hizWidth = m_hizHeight = m_hizMipCount = 1;
	m_hizEnabled = false;
	m_debugLastGpuVisibleCount = 0;
	m_lastDrawIndirectBatchCount = 0;

	m_computeRootSig.Reset();
	m_computePso.Reset();
	m_cmdSig.Reset();

	m_cullCBUpload.Reset();
	m_treeInfoDefault.Reset();
	m_treeInfoUpload.Reset();
	m_instanceDataDefault.Reset();
	m_instanceDataUpload.Reset();
	for (int i = 0; i < kBatchCount; ++i)
	{
		m_visibleIndexDefault[i].Reset();
		m_indirectArgsDefault[i].Reset();
		m_counterDefault[i].Reset();
		m_uavVisible[i] = nullptr;
		m_uavArgs[i] = nullptr;
		m_uavCounter[i] = nullptr;
		m_visibleState[i] = static_cast<D3D12_RESOURCE_STATES>(0);
		m_indirectState[i] = static_cast<D3D12_RESOURCE_STATES>(0);
		m_counterState[i] = static_cast<D3D12_RESOURCE_STATES>(0);
	}
	m_indirectResetUpload.Reset();
	m_counterResetUpload.Reset();

	m_srvTreeInfo = nullptr;

	m_infoState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_instanceState = static_cast<D3D12_RESOURCE_STATES>(0);
}

bool TreeGpuCullSystem::CreateIndirectCommandSignature(ID3D12Device* device, ID3D12RootSignature* pbrRootSignature)
{
	(void)pbrRootSignature;
	D3D12_INDIRECT_ARGUMENT_DESC args[1] = {};
	args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC sig = {};
	sig.ByteStride = kTreeIndirectStrideBytes;
	sig.NumArgumentDescs = _countof(args);
	sig.pArgumentDescs = args;

	// No root arguments are changed by this signature.
	const HRESULT hr = device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(m_cmdSig.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

bool TreeGpuCullSystem::CreatePipelines(ID3D12Device* device)
{
	if (!device)
		return false;

	ComPtr<ID3DBlob> csBlob;
	if (!LoadCsBlob(L"TreeFrustumHiZCull_CS.cso", L"Shaders\\TreeFrustumHiZCull_CS.cso", csBlob))
	{
		DebugLog("[TreeGpuCull] TreeFrustumHiZCull_CS.cso not found\n");
		return false;
	}

	CD3DX12_ROOT_PARAMETER rootParams[4] = {};
	CD3DX12_DESCRIPTOR_RANGE srvRange[1] = {};
	CD3DX12_DESCRIPTOR_RANGE uavRangesBoth[3] = {};

	// b0: CB, t0: TreeInfo buffer (SRV), t1: HiZ texture (SRV),
	// u0..u26: visibleIndex, u27..u53: indirect args, u54..u80: counters
	rootParams[0].InitAsConstantBufferView(0);
	rootParams[1].InitAsShaderResourceView(0);
	srvRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1
	rootParams[2].InitAsDescriptorTable(1, &srvRange[0]);
	uavRangesBoth[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kBatchCount, 0);            // u0..u26   visibleIndex
	uavRangesBoth[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kBatchCount, kBatchCount); // u27..u53  args
	uavRangesBoth[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kBatchCount, kBatchCount * 2); // u54..u80 counter
	rootParams[3].InitAsDescriptorTable(3, uavRangesBoth);

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = _countof(rootParams);
	rsDesc.pParameters = rootParams;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsBlob, rsErr;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr))
		return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_computeRootSig.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = m_computeRootSig.Get();
	pso.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_computePso.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

bool TreeGpuCullSystem::Init(ID3D12Device* device, DescriptorHeap* descriptorHeap, ID3D12RootSignature* pbrRootSignature, uint32_t maxInstances)
{
	Shutdown();
	if (!device || !descriptorHeap || !pbrRootSignature || maxInstances == 0)
		return false;

	m_descriptorHeap = descriptorHeap;
	m_maxInstances = maxInstances;

	if (!CreatePipelines(device) || !CreateIndirectCommandSignature(device, pbrRootSignature))
	{
		DebugLog("[TreeGpuCull] pipeline or command signature failed\n");
		return false;
	}

	auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	const UINT64 infoBytes = sizeof(TreeInfoGpu) * static_cast<UINT64>(m_maxInstances);
	const UINT64 instBytes = sizeof(InstanceData) * static_cast<UINT64>(m_maxInstances);
	const UINT64 visibleBytes = sizeof(uint32_t) * static_cast<UINT64>(m_maxInstances);
	const UINT64 indirectBytes = static_cast<UINT64>(kTreeIndirectStrideBytes); // 1 cmd per batch
	const UINT64 counterBytes = sizeof(uint32_t);

	auto infoDesc = CD3DX12_RESOURCE_DESC::Buffer(infoBytes);
	auto infoUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(infoBytes);
	auto instDesc = CD3DX12_RESOURCE_DESC::Buffer(instBytes);
	auto instUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(instBytes);
	auto visibleDesc = CD3DX12_RESOURCE_DESC::Buffer(visibleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto indirectDesc = CD3DX12_RESOURCE_DESC::Buffer(indirectBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto counterDesc = CD3DX12_RESOURCE_DESC::Buffer(counterBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto indirectResetUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(indirectBytes * kBatchCount);
	auto counterResetUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(counterBytes);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(TreeCullCB));

	// NOTE: D3D12 buffers ignore InitialState and start effectively as COMMON (debug layer warning #1328).
	HRESULT hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &infoDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_treeInfoDefault.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &infoUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_treeInfoUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &instDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_instanceDataDefault.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &instUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_instanceDataUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	for (int i = 0; i < kBatchCount; ++i)
	{
		hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &visibleDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_visibleIndexDefault[i].ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
		hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &indirectDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_indirectArgsDefault[i].ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
		hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &counterDesc, static_cast<D3D12_RESOURCE_STATES>(0), nullptr, IID_PPV_ARGS(m_counterDefault[i].ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
	}
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &indirectResetUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_indirectResetUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &counterResetUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_counterResetUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	{
		TreeIndirectCmdGpu zero{};
		void* p = nullptr;
		if (SUCCEEDED(m_indirectResetUpload->Map(0, nullptr, &p)) && p)
		{
			memcpy(p, &zero, sizeof(zero));
			m_indirectResetUpload->Unmap(0, nullptr);
		}
	}
	{
		uint32_t zero = 0;
		void* p = nullptr;
		if (SUCCEEDED(m_counterResetUpload->Map(0, nullptr, &p)) && p)
		{
			memcpy(p, &zero, sizeof(zero));
			m_counterResetUpload->Unmap(0, nullptr);
		}
	}

	hr = device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_cullCBUpload.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	// NOTE: CPU readback counters removed (GPU-only pipeline).

	// SRV for TreeInfoGpu
	D3D12_SHADER_RESOURCE_VIEW_DESC srvInfo = {};
	srvInfo.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvInfo.Format = DXGI_FORMAT_UNKNOWN;
	srvInfo.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvInfo.Buffer.FirstElement = 0;
	srvInfo.Buffer.NumElements = m_maxInstances;
	srvInfo.Buffer.StructureByteStride = sizeof(TreeInfoGpu);
	srvInfo.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	m_srvTreeInfo = descriptorHeap->RegisterResource(m_treeInfoDefault.Get(), srvInfo);
	if (!m_srvTreeInfo)
		return false;

	// IMPORTANT: The compute shader uses a single UAV descriptor table with 2 ranges:
	// 1) u0..u26  : visible index buffers
	// 2) u27..u53 : indirect args buffers
	// Therefore, the descriptors MUST be contiguous in the heap in that order.
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavVis = {};
	uavVis.Format = DXGI_FORMAT_UNKNOWN;
	uavVis.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavVis.Buffer.FirstElement = 0;
	uavVis.Buffer.NumElements = m_maxInstances;
	uavVis.Buffer.StructureByteStride = sizeof(uint32_t);
	uavVis.Buffer.CounterOffsetInBytes = 0;
	uavVis.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	for (int bi = 0; bi < kBatchCount; ++bi)
	{
		m_uavVisible[bi] = descriptorHeap->CreateUAV(m_visibleIndexDefault[bi].Get(), uavVis);
		if (!m_uavVisible[bi])
			return false;
	}

	// UAV for indirect args (raw structured bytes)
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavArgs = {};
	uavArgs.Format = DXGI_FORMAT_UNKNOWN;
	uavArgs.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavArgs.Buffer.FirstElement = 0;
	uavArgs.Buffer.NumElements = 1;
	uavArgs.Buffer.StructureByteStride = kTreeIndirectStrideBytes;
	uavArgs.Buffer.CounterOffsetInBytes = 0;
	uavArgs.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	for (int bi = 0; bi < kBatchCount; ++bi)
	{
		m_uavArgs[bi] = descriptorHeap->CreateUAV(m_indirectArgsDefault[bi].Get(), uavArgs);
		if (!m_uavArgs[bi])
			return false;
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavCounter = {};
	uavCounter.Format = DXGI_FORMAT_UNKNOWN;
	uavCounter.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavCounter.Buffer.FirstElement = 0;
	uavCounter.Buffer.NumElements = 1;
	uavCounter.Buffer.StructureByteStride = sizeof(uint32_t);
	uavCounter.Buffer.CounterOffsetInBytes = 0;
	uavCounter.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	for (int bi = 0; bi < kBatchCount; ++bi)
	{
		m_uavCounter[bi] = descriptorHeap->CreateUAV(m_counterDefault[bi].Get(), uavCounter);
		if (!m_uavCounter[bi])
			return false;
	}

	// Hi-Z fallback SRV: always provide a valid texture so DispatchCull can run even when Hi-Z is disabled.
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

	// NOTE: D3D12 buffers ignore InitialState and start effectively as COMMON (see warning #1328).
	m_infoState = static_cast<D3D12_RESOURCE_STATES>(0);
	m_instanceState = static_cast<D3D12_RESOURCE_STATES>(0);
	for (int bi = 0; bi < kBatchCount; ++bi)
	{
		m_visibleState[bi] = static_cast<D3D12_RESOURCE_STATES>(0);
		m_indirectState[bi] = static_cast<D3D12_RESOURCE_STATES>(0);
		m_counterState[bi] = static_cast<D3D12_RESOURCE_STATES>(0);
	}

	{
		auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t) * kBatchCount);
		auto heapReadback = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
		hr = device->CreateCommittedResource(&heapReadback, D3D12_HEAP_FLAG_NONE, &readbackDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(m_counterReadback.ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
	}
	{
		auto argsReadbackDesc = CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(kTreeIndirectStrideBytes) * kBatchCount);
		auto heapReadback = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
		hr = device->CreateCommittedResource(&heapReadback, D3D12_HEAP_FLAG_NONE, &argsReadbackDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(m_indirectArgsReadback.ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
	}

	m_valid = true;
	return true;
}

void TreeGpuCullSystem::SetHiZResources(DescriptorHandle* hizSrv, uint32_t hizWidth, uint32_t hizHeight, uint32_t hizMipCount, bool enabled)
{
	m_hizSrv = hizSrv ? hizSrv : m_hizFallbackSrv;
	m_hizWidth = (hizWidth == 0u) ? 1u : hizWidth;
	m_hizHeight = (hizHeight == 0u) ? 1u : hizHeight;
	m_hizMipCount = (hizMipCount == 0u) ? 1u : hizMipCount;
	m_hizEnabled = enabled && (hizSrv != nullptr);
}

void TreeGpuCullSystem::UpdateInstances(ID3D12GraphicsCommandList* cmd, const TreeInstanceCpu* instances, uint32_t count)
{
	if (!m_valid || !cmd || !instances || count == 0)
	{
		m_instanceCount = 0;
		return;
	}
	m_instanceCount = (count > m_maxInstances) ? m_maxInstances : count;

	void* pInfo = nullptr;
	void* pInst = nullptr;
	if (FAILED(m_treeInfoUpload->Map(0, nullptr, &pInfo)) || !pInfo)
	{
		m_instanceCount = 0;
		return;
	}
	if (FAILED(m_instanceDataUpload->Map(0, nullptr, &pInst)) || !pInst)
	{
		m_treeInfoUpload->Unmap(0, nullptr);
		m_instanceCount = 0;
		return;
	}

	TreeInfoGpu* outInfo = reinterpret_cast<TreeInfoGpu*>(pInfo);
	InstanceData* outInst = reinterpret_cast<InstanceData*>(pInst);
	const D3D12_GPU_VIRTUAL_ADDRESS instanceDataBaseVA = m_instanceDataDefault->GetGPUVirtualAddress();
	const unsigned hw = std::thread::hardware_concurrency();
	const unsigned threadCount = (hw > 1u) ? hw : 1u;
	if (threadCount == 1u || m_instanceCount < 8192u)
	{
		BuildUploadInstanceRange(instances, 0u, m_instanceCount, outInfo, outInst, instanceDataBaseVA);
	}
	else
	{
		const uint32_t chunk = (m_instanceCount + threadCount - 1u) / threadCount;
		std::vector<std::thread> workers;
		workers.reserve(threadCount);
		for (unsigned t = 0; t < threadCount; ++t)
		{
			const uint32_t begin = static_cast<uint32_t>(t) * chunk;
			const uint32_t end = (begin + chunk < m_instanceCount) ? (begin + chunk) : m_instanceCount;
			if (begin >= end)
				break;
			workers.emplace_back([instances, begin, end, outInfo, outInst, instanceDataBaseVA]() {
				BuildUploadInstanceRange(instances, begin, end, outInfo, outInst, instanceDataBaseVA);
			});
		}
		for (auto& w : workers)
			w.join();
	}

	m_instanceDataUpload->Unmap(0, nullptr);
	m_treeInfoUpload->Unmap(0, nullptr);

	// Copy to default
	if (m_infoState != D3D12_RESOURCE_STATE_COPY_DEST || m_instanceState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		D3D12_RESOURCE_BARRIER bb[2] = {};
		UINT bi = 0;
		if (m_infoState != D3D12_RESOURCE_STATE_COPY_DEST)
			bb[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(m_treeInfoDefault.Get(), m_infoState, D3D12_RESOURCE_STATE_COPY_DEST);
		if (m_instanceState != D3D12_RESOURCE_STATE_COPY_DEST)
			bb[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(m_instanceDataDefault.Get(), m_instanceState, D3D12_RESOURCE_STATE_COPY_DEST);
		if (bi > 0)
			cmd->ResourceBarrier(bi, bb);
		m_infoState = D3D12_RESOURCE_STATE_COPY_DEST;
		m_instanceState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	cmd->CopyBufferRegion(m_treeInfoDefault.Get(), 0, m_treeInfoUpload.Get(), 0, sizeof(TreeInfoGpu) * static_cast<UINT64>(m_instanceCount));
	cmd->CopyBufferRegion(m_instanceDataDefault.Get(), 0, m_instanceDataUpload.Get(), 0, sizeof(InstanceData) * static_cast<UINT64>(m_instanceCount));

	{
		D3D12_RESOURCE_BARRIER bbb[2] = {};
		bbb[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_treeInfoDefault.Get(), m_infoState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		bbb[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_instanceDataDefault.Get(), m_instanceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		cmd->ResourceBarrier(2, bbb);
		m_infoState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		m_instanceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}
}

void TreeGpuCullSystem::SetTreeLodDistanceTuning(float lod1StartMeters, float lod2StartMeters)
{
	m_lod1StartDistance = std::max(0.01f, lod1StartMeters);
	m_lod2StartDistance = std::max(m_lod1StartDistance + 0.1f, lod2StartMeters);
}

void TreeGpuCullSystem::GetTreeLodDistanceTuning(float& outLod1StartMeters, float& outLod2StartMeters) const
{
	outLod1StartMeters = m_lod1StartDistance;
	outLod2StartMeters = m_lod2StartDistance;
}

void TreeGpuCullSystem::ComputeDebugDistanceLodCounts(
	const TreeInstanceCpu* instances,
	uint32_t count,
	const DirectX::XMFLOAT3& cameraWorldPos,
	float lod1StartMeters,
	float lod2StartMeters,
	uint32_t& outLod0,
	uint32_t& outLod1,
	uint32_t& outLod2)
{
	outLod0 = 0;
	outLod1 = 0;
	outLod2 = 0;
	if (!instances || count == 0)
		return;

	const float l1 = std::max(0.01f, lod1StartMeters);
	const float l2 = std::max(l1 + 0.1f, lod2StartMeters);

	for (uint32_t i = 0; i < count; ++i)
	{
		const XMMATRIX worldRow = XMMatrixTranspose(instances[i].worldGpuT);
		XMFLOAT4X4 wr{};
		XMStoreFloat4x4(&wr, worldRow);
		// シェーダ TreeFrustumHiZCull_CS と同じく XZ のみ（地上カメラ高でも「横の遠さ」で LOD する）
		const float dx = cameraWorldPos.x - wr._41;
		const float dz = cameraWorldPos.z - wr._43;
		const float dist = sqrtf(dx * dx + dz * dz);

		if (dist >= l2)
			++outLod2;
		else if (dist >= l1)
			++outLod1;
		else
			++outLod0;
	}
}

bool TreeGpuCullSystem::DispatchCull(
	ID3D12GraphicsCommandList* cmd,
	const SceneConstants* scene,
	const uint32_t indexCountByPartByLod[3][3],
	const DirectX::XMFLOAT3& cameraWorldPos)
{
	if (!m_valid || !cmd || !scene || m_instanceCount == 0 || !m_srvTreeInfo || !m_uavArgs[0] || !m_uavVisible[0] || !m_uavCounter[0] || !m_hizSrv)
		return false;

	const XMMATRIX view = XMMatrixTranspose(scene->View);
	const XMMATRIX proj = XMMatrixTranspose(scene->Proj);
	const XMMATRIX vp = XMMatrixMultiply(view, proj);

	TreeCullCB cb{};
	cb.CameraPos = XMFLOAT4(cameraWorldPos.x, cameraWorldPos.y, cameraWorldPos.z, 1.f);
	cb.Params = XMFLOAT4(static_cast<float>(m_instanceCount), 0.f, 0.f, 0.f);
	ExtractFrustumPlanesForTreeGpu(vp, cb.FrustumPlanes);
	cb.View = scene->View;
	cb.Proj = scene->Proj;

	// Diagnostic: CPU-side frustum test for first tree instance
	{
		static int s_diagCount = 0;
		if (s_diagCount < 3 && m_instanceCount > 0)
		{
			++s_diagCount;
			void* pDiag = nullptr;
			if (SUCCEEDED(m_treeInfoUpload->Map(0, nullptr, &pDiag)) && pDiag)
			{
				const TreeInfoGpu* info0 = reinterpret_cast<const TreeInfoGpu*>(pDiag);
				const XMFLOAT3 c(info0->centerRadius.x, info0->centerRadius.y, info0->centerRadius.z);
				const float r = info0->centerRadius.w;
				DebugLog("[TreeGpuCull][Diag] inst0 center=(%.2f,%.2f,%.2f) r=%.2f\n", c.x, c.y, c.z, r);
				for (int p = 0; p < 6; ++p)
				{
					const XMFLOAT4& pl = cb.FrustumPlanes[p];
					const float len = sqrtf(pl.x * pl.x + pl.y * pl.y + pl.z * pl.z);
					const float d = pl.x * c.x + pl.y * c.y + pl.z * c.z + pl.w;
					const bool outside = (len > 1e-5f) && (d > r * len);
					DebugLog("  plane[%d] n=(%.4f,%.4f,%.4f) w=%.4f len=%.4f d=%.4f d/len=%.4f outside=%s\n",
						p, pl.x, pl.y, pl.z, pl.w, len, d, (len > 1e-5f ? d / len : 0.f),
						outside ? "YES" : "no");
				}
				const float radius = CullSphereRadiusFromMergedTreeBounds();
				const ModelBounds& mb = TreeVegetation::GetMergedLocalBounds();
				DebugLog("[TreeGpuCull][Diag] mergedBounds=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) radius=%.2f\n",
					mb.Min.x, mb.Min.y, mb.Min.z, mb.Max.x, mb.Max.y, mb.Max.z, radius);

				// Test a synthetic point 1000m behind the camera
				const XMFLOAT3 fwd(cb.CameraPos.x + 1000.f * (-0.7f), cb.CameraPos.y, cb.CameraPos.z + 1000.f * (-0.7f));
				const XMFLOAT3 behind(cb.CameraPos.x - 1000.f * (-0.7f), cb.CameraPos.y, cb.CameraPos.z - 1000.f * (-0.7f));
				for (int test = 0; test < 2; ++test)
				{
					const XMFLOAT3& tp = (test == 0) ? fwd : behind;
					int outsideCount = 0;
					for (int p2 = 0; p2 < 6; ++p2)
					{
						const XMFLOAT4& pl = cb.FrustumPlanes[p2];
						const float len2 = sqrtf(pl.x*pl.x + pl.y*pl.y + pl.z*pl.z);
						const float d2 = pl.x*tp.x + pl.y*tp.y + pl.z*tp.z + pl.w;
						if (len2 > 1e-5f && d2 > 10.f * len2) ++outsideCount;
					}
					DebugLog("[TreeGpuCull][Diag] testPt %s (%.0f,%.0f,%.0f) outsidePlanes=%d → %s\n",
						test == 0 ? "FRONT" : "BEHIND", tp.x, tp.y, tp.z, outsideCount,
						outsideCount > 0 ? "CULLED" : "NOT_CULLED");
				}

				m_treeInfoUpload->Unmap(0, nullptr);
			}
		}
	}
	cb.HiZParams = XMFLOAT4(m_hizEnabled ? 1.f : 0.f, static_cast<float>(m_hizWidth), static_cast<float>(m_hizHeight), static_cast<float>(m_hizMipCount));
	cb.HiZTuning = XMFLOAT4(150.0f, 0.01f, 96.0f, 0.0f);
	cb.IndexCountsTrunk = XMUINT4(indexCountByPartByLod[0][0], indexCountByPartByLod[0][1], indexCountByPartByLod[0][2], 0u);
	cb.IndexCountsLeaves = XMUINT4(indexCountByPartByLod[1][0], indexCountByPartByLod[1][1], indexCountByPartByLod[1][2], 0u);
	cb.IndexCountsBranches = XMUINT4(indexCountByPartByLod[2][0], indexCountByPartByLod[2][1], indexCountByPartByLod[2][2], 0u);
	// LOD distances (meters, XZ): dist < x → LOD0; dist < y → LOD1; else LOD2
	cb.LodParams = XMFLOAT4(m_lod1StartDistance, m_lod2StartDistance, m_maxDrawDistance, 0.0f);

	void* p = nullptr;
	if (FAILED(m_cullCBUpload->Map(0, nullptr, &p)) || !p)
		return false;
	memcpy(p, &cb, sizeof(cb));
	m_cullCBUpload->Unmap(0, nullptr);

	// Initialize one indirect command per batch from CPU.
	// Write ALL batch args to the upload buffer in one Map, then copy each from its offset.
	{
		void* pAll = nullptr;
		if (FAILED(m_indirectResetUpload->Map(0, nullptr, &pAll)) || !pAll)
			return false;
		auto* dst = reinterpret_cast<TreeIndirectCmdGpu*>(pAll);
		for (int species = 0; species < kSpeciesCount; ++species)
		{
			for (int lod = 0; lod < kLodCount; ++lod)
			{
				for (int part = 0; part < kPartCount; ++part)
				{
					const int bi = BatchIndex(species, lod, part);
					dst[bi].draw.IndexCountPerInstance = indexCountByPartByLod[part][lod];
					dst[bi].draw.InstanceCount = kTreeDebugForceOneInstancePerBatch ? 1u : 0u;
					dst[bi].draw.StartIndexLocation = 0;
					dst[bi].draw.BaseVertexLocation = 0;
					dst[bi].draw.StartInstanceLocation = 0;
				}
			}
		}
		m_indirectResetUpload->Unmap(0, nullptr);

		for (int bi = 0; bi < kBatchCount; ++bi)
		{
			if (m_indirectState[bi] != D3D12_RESOURCE_STATE_COPY_DEST)
			{
				CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					m_indirectArgsDefault[bi].Get(),
					m_indirectState[bi],
					D3D12_RESOURCE_STATE_COPY_DEST);
				cmd->ResourceBarrier(1, &b);
				m_indirectState[bi] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			cmd->CopyBufferRegion(m_indirectArgsDefault[bi].Get(), 0,
				m_indirectResetUpload.Get(), static_cast<UINT64>(bi) * sizeof(TreeIndirectCmdGpu),
				sizeof(TreeIndirectCmdGpu));
		}
	}

	// Reset per-batch counters to 0.
	for (int bi = 0; bi < kBatchCount; ++bi)
	{
		if (m_counterState[bi] != D3D12_RESOURCE_STATE_COPY_DEST)
		{
			CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(m_counterDefault[bi].Get(), m_counterState[bi], D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->ResourceBarrier(1, &b);
			m_counterState[bi] = D3D12_RESOURCE_STATE_COPY_DEST;
		}
		cmd->CopyBufferRegion(m_counterDefault[bi].Get(), 0, m_counterResetUpload.Get(), 0, sizeof(uint32_t));
	}

	// Transition args + visible + counters to UAV for the cull shader.
	{
		std::vector<D3D12_RESOURCE_BARRIER> bb;
		bb.reserve(static_cast<size_t>(kBatchCount) * 3u);
		for (int bi = 0; bi < kBatchCount; ++bi)
		{
			if (m_indirectState[bi] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			{
				bb.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_indirectArgsDefault[bi].Get(), m_indirectState[bi], D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
				m_indirectState[bi] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			if (m_visibleState[bi] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			{
				bb.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_visibleIndexDefault[bi].Get(), m_visibleState[bi], D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
				m_visibleState[bi] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			if (m_counterState[bi] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			{
				bb.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_counterDefault[bi].Get(), m_counterState[bi], D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
				m_counterState[bi] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			}
		}
		if (!bb.empty())
			cmd->ResourceBarrier(static_cast<UINT>(bb.size()), bb.data());
	}

	cmd->SetPipelineState(m_computePso.Get());
	cmd->SetComputeRootSignature(m_computeRootSig.Get());

	ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap->GetHeap() };
	cmd->SetDescriptorHeaps(1, heaps);

	cmd->SetComputeRootConstantBufferView(0, m_cullCBUpload->GetGPUVirtualAddress());
	cmd->SetComputeRootShaderResourceView(1, m_treeInfoDefault->GetGPUVirtualAddress());
	cmd->SetComputeRootDescriptorTable(2, m_hizSrv->HandleGPU);
	// RootParam[3] is a descriptor table with 2 UAV ranges:
	// u0..u26 visibleIndex, u27..u53 args.
	cmd->SetComputeRootDescriptorTable(3, m_uavVisible[0]->HandleGPU);

	const UINT groups = (m_instanceCount + 63u) / 64u;
	cmd->Dispatch(groups, 1, 1);

	{
		std::vector<D3D12_RESOURCE_BARRIER> ub;
		ub.reserve(static_cast<size_t>(kBatchCount) * 3u);
		for (int bi = 0; bi < kBatchCount; ++bi)
			ub.push_back(CD3DX12_RESOURCE_BARRIER::UAV(m_visibleIndexDefault[bi].Get()));
		for (int bi = 0; bi < kBatchCount; ++bi)
			ub.push_back(CD3DX12_RESOURCE_BARRIER::UAV(m_indirectArgsDefault[bi].Get()));
		for (int bi = 0; bi < kBatchCount; ++bi)
			ub.push_back(CD3DX12_RESOURCE_BARRIER::UAV(m_counterDefault[bi].Get()));
		if (!ub.empty())
			cmd->ResourceBarrier(static_cast<UINT>(ub.size()), ub.data());
	}

	// Copy counter -> indirectArgs.draw.InstanceCount (offset +4) for each batch.
	// In debug force mode, keep instanceCount=1 to validate draw path end-to-end.
	if (!kTreeDebugForceOneInstancePerBatch)
	{
		for (int bi = 0; bi < kBatchCount; ++bi)
		{
			D3D12_RESOURCE_BARRIER bb[2] = {};
			UINT nbar = 0;
			if (m_counterState[bi] != D3D12_RESOURCE_STATE_COPY_SOURCE)
			{
				bb[nbar++] = CD3DX12_RESOURCE_BARRIER::Transition(m_counterDefault[bi].Get(), m_counterState[bi], D3D12_RESOURCE_STATE_COPY_SOURCE);
				m_counterState[bi] = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (m_indirectState[bi] != D3D12_RESOURCE_STATE_COPY_DEST)
			{
				bb[nbar++] = CD3DX12_RESOURCE_BARRIER::Transition(m_indirectArgsDefault[bi].Get(), m_indirectState[bi], D3D12_RESOURCE_STATE_COPY_DEST);
				m_indirectState[bi] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (nbar > 0)
				cmd->ResourceBarrier(nbar, bb);

			cmd->CopyBufferRegion(
				m_indirectArgsDefault[bi].Get(),
				offsetof(TreeIndirectCmdGpu, draw) + offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount),
				m_counterDefault[bi].Get(),
				0,
				sizeof(uint32_t));
		}
	}

	if (m_counterReadback)
	{
		if (m_counterReadbackPending)
		{
			void* pRead = nullptr;
			D3D12_RANGE readRange = { 0, sizeof(uint32_t) * kBatchCount };
			if (SUCCEEDED(m_counterReadback->Map(0, &readRange, &pRead)) && pRead)
			{
				const uint32_t* vals = reinterpret_cast<const uint32_t*>(pRead);
				uint32_t sumLod[3] = {};
				for (int sp = 0; sp < kSpeciesCount; ++sp)
					for (int lod = 0; lod < kLodCount; ++lod)
						for (int pt = 0; pt < kPartCount; ++pt)
							sumLod[lod] += vals[BatchIndex(sp, lod, pt)];
				m_lastReadbackLod0 = sumLod[0];
				m_lastReadbackLod1 = sumLod[1];
				m_lastReadbackLod2 = sumLod[2];
				static int s_logCount = 0;
				if (s_logCount < 10)
				{
					DebugLog("[TreeGpuCull][CounterReadback] LOD0=%u LOD1=%u LOD2=%u (total=%u)\n",
						sumLod[0], sumLod[1], sumLod[2], sumLod[0] + sumLod[1] + sumLod[2]);
					++s_logCount;
				}
				D3D12_RANGE noWrite = { 0, 0 };
				m_counterReadback->Unmap(0, &noWrite);
			}

			if (m_indirectArgsReadback)
			{
				void* pArgs = nullptr;
				D3D12_RANGE argsRange = { 0, static_cast<SIZE_T>(kTreeIndirectStrideBytes) * kBatchCount };
				if (SUCCEEDED(m_indirectArgsReadback->Map(0, &argsRange, &pArgs)) && pArgs)
				{
					static int s_argsLog = 0;
					if (s_argsLog < 10)
					{
						const auto* base = reinterpret_cast<const uint8_t*>(pArgs);
						for (int sp = 0; sp < kSpeciesCount; ++sp)
						{
							for (int lod = 0; lod < kLodCount; ++lod)
							{
								const int bi = BatchIndex(sp, lod, 0);
								const auto* args = reinterpret_cast<const D3D12_DRAW_INDEXED_ARGUMENTS*>(
									base + static_cast<size_t>(bi) * kTreeIndirectStrideBytes);
								if (args->InstanceCount > 0)
								{
									DebugLog("[TreeGpuCull][ArgsReadback] sp=%d lod=%d: IdxCount=%u InstCount=%u startIdx=%u baseVtx=%d startInst=%u\n",
										sp, lod, args->IndexCountPerInstance, args->InstanceCount,
										args->StartIndexLocation, args->BaseVertexLocation, args->StartInstanceLocation);
								}
							}
						}
						++s_argsLog;
					}
					D3D12_RANGE noWrite = { 0, 0 };
					m_indirectArgsReadback->Unmap(0, &noWrite);
				}
			}
		}

		for (int bi = 0; bi < kBatchCount; ++bi)
		{
			D3D12_RESOURCE_BARRIER rb = CD3DX12_RESOURCE_BARRIER::Transition(
				m_counterDefault[bi].Get(), m_counterState[bi], D3D12_RESOURCE_STATE_COPY_SOURCE);
			if (m_counterState[bi] != D3D12_RESOURCE_STATE_COPY_SOURCE)
			{
				cmd->ResourceBarrier(1, &rb);
				m_counterState[bi] = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			cmd->CopyBufferRegion(m_counterReadback.Get(), sizeof(uint32_t) * bi, m_counterDefault[bi].Get(), 0, sizeof(uint32_t));
		}

		for (int bi = 0; bi < kBatchCount; ++bi)
		{
			if (m_indirectState[bi] != D3D12_RESOURCE_STATE_COPY_SOURCE)
			{
				D3D12_RESOURCE_BARRIER rb = CD3DX12_RESOURCE_BARRIER::Transition(
					m_indirectArgsDefault[bi].Get(), m_indirectState[bi], D3D12_RESOURCE_STATE_COPY_SOURCE);
				cmd->ResourceBarrier(1, &rb);
				m_indirectState[bi] = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			cmd->CopyBufferRegion(m_indirectArgsReadback.Get(),
				static_cast<UINT64>(bi) * kTreeIndirectStrideBytes,
				m_indirectArgsDefault[bi].Get(), 0, kTreeIndirectStrideBytes);
		}
		m_counterReadbackPending = true;
	}

	return true;
}

void TreeGpuCullSystem::DrawIndirectLods(
	ID3D12GraphicsCommandList* cmd,
	RootSignature* pbrRootSig,
	PipelineState* psoOpaque,
	PipelineState* psoLeavesAlphaCut,
	PipelineState* psoImposterLod1,
	PipelineState* psoLod0DepthPrepass,
	D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
	D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
	const D3D12_GPU_DESCRIPTOR_HANDLE matBySpeciesByPartByLod[3][3][3],
	const D3D12_GPU_DESCRIPTOR_HANDLE imposterMatTableBySpecies[3],
	D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
	VertexBuffer* vbByPartByLod[3][3],
	IndexBuffer* ibByPartByLod[3][3],
	const uint32_t indexCountByPartByLod[3][3],
	VertexBuffer* vbImposterQuad,
	IndexBuffer* ibImposterQuad)
{
	if (!m_valid || !cmd || !pbrRootSig || !pbrRootSig->IsValid() || !m_cmdSig)
	{
		m_lastDrawIndirectBatchCount = 0;
		return;
	}

	m_debugLastGpuVisibleCount = 0;
	// NOTE: GPU-only pipeline; previous-frame readback counters are not used.
	uint32_t dbgEligible = 0;
	uint32_t dbgSkipIdx0 = 0;
	uint32_t dbgSkipVb0 = 0;
	uint32_t dbgSkipIb0 = 0;
	uint32_t dbgSkipMat0 = 0;
	uint32_t dbgExecuteCalls = 0;

	// FBX マージ1本で 3 パート同一 IB のとき：CS は part0 バッチにだけ可視を積む。描画も part0 を1回にまとめる。
	// 以前は TreeLOD1_PS + 葉マテで1パスにしていたが、幹のUVで葉用 alpha を clip し LOD0 が全消しになるため、
	// マージ LOD0 は幹マテ + 不透明 StandardPBR にする（葉三角形は幹テクスチャでやや誤表示。完全表示はパート分割が必要）。
	const bool mergedLod0Triple =
		indexCountByPartByLod[0][0] > 0u &&
		indexCountByPartByLod[0][0] == indexCountByPartByLod[1][0] &&
		indexCountByPartByLod[1][0] == indexCountByPartByLod[2][0];

	// --- LOD0 depth prepass: write Z only, no pixel shader ---
	const bool doLod0DepthPrepass = psoLod0DepthPrepass && psoLod0DepthPrepass->IsValid();
	if (doLod0DepthPrepass)
	{
		for (int species = 0; species < kSpeciesCount; ++species)
		{
			const int lod = 0;
			const int part = 0;
			if (m_debugSkipLod0)
				break;

			const uint32_t idxCount = indexCountByPartByLod[part][lod];
			VertexBuffer* vb = vbByPartByLod[part][lod];
			IndexBuffer* ib = ibByPartByLod[part][lod];
			if (idxCount == 0 || !vb || !ib)
				continue;
			const D3D12_GPU_DESCRIPTOR_HANDLE mat = matBySpeciesByPartByLod[species][0][lod];
			if (mat.ptr == 0)
				continue;

			const int batch = BatchIndex(species, lod, part);

			if (m_indirectState[batch] != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
				|| m_visibleState[batch] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			{
				D3D12_RESOURCE_BARRIER bb[2] = {};
				UINT bi = 0;
				if (m_indirectState[batch] != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
					bb[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(m_indirectArgsDefault[batch].Get(), m_indirectState[batch], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
				if (m_visibleState[batch] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
					bb[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(m_visibleIndexDefault[batch].Get(), m_visibleState[batch], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				if (bi > 0)
					cmd->ResourceBarrier(bi, bb);
				m_indirectState[batch] = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
				m_visibleState[batch] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}

			cmd->SetPipelineState(psoLod0DepthPrepass->Get());
			cmd->SetGraphicsRootSignature(pbrRootSig->Get());
			cmd->SetGraphicsRootConstantBufferView(0, sceneCbGpu);
			cmd->SetGraphicsRootConstantBufferView(1, materialCbGpu);
			cmd->SetGraphicsRootShaderResourceView(2, m_instanceDataDefault->GetGPUVirtualAddress());
			cmd->SetGraphicsRootShaderResourceView(5, m_visibleIndexDefault[batch]->GetGPUVirtualAddress());
			uint32_t treeVisConstants[8];
			PackTreeVisibleRootConstants(treeVisConstants, 0u, XMFLOAT3(0.f, 0.f, 0.f), 0.f, 0.f);
			cmd->SetGraphicsRoot32BitConstants(6, 8, treeVisConstants, 0);
			cmd->SetGraphicsRootDescriptorTable(3, mat);
			if (iblTable.ptr != 0)
				cmd->SetGraphicsRootDescriptorTable(4, iblTable);

			D3D12_VERTEX_BUFFER_VIEW vbView = vb->View();
			D3D12_INDEX_BUFFER_VIEW ibView = ib->View();
			cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmd->IASetVertexBuffers(0, 1, &vbView);
			cmd->IASetIndexBuffer(&ibView);
			cmd->ExecuteIndirect(m_cmdSig.Get(), 1, m_indirectArgsDefault[batch].Get(), 0, nullptr, 0);
		}
	}

	for (int species = 0; species < kSpeciesCount; ++species)
	{
		for (int lod = 0; lod < kLodCount; ++lod)
		{
			for (int part = 0; part < kPartCount; ++part)
			{
				// LOD1/LOD2 are imposter batches (part0 only). part1/2 have no CS output.
				if (lod >= 1 && part != 0)
					continue;
				if (lod == 0 && mergedLod0Triple && (part == 1 || part == 2))
					continue;
				if (lod == 0 && m_debugSkipLod0)
					continue;

				const bool useImposterLod1 = (lod >= 1 && part == 0 && psoImposterLod1 && psoImposterLod1->IsValid()
					&& vbImposterQuad && ibImposterQuad && vbImposterQuad->IsValid() && ibImposterQuad->IsValid()
					&& imposterMatTableBySpecies[species].ptr != 0
					&& indexCountByPartByLod[0][lod] == 6u);
				const bool useMergedLod0OpaquePbr = (lod == 0 && part == 0 && mergedLod0Triple);

				const uint32_t idxCount = indexCountByPartByLod[part][lod];
				VertexBuffer* vb = useImposterLod1 ? vbImposterQuad : vbByPartByLod[part][lod];
				IndexBuffer* ib = useImposterLod1 ? ibImposterQuad : ibByPartByLod[part][lod];
				const D3D12_GPU_DESCRIPTOR_HANDLE mat = useImposterLod1
					? imposterMatTableBySpecies[species]
					: (useMergedLod0OpaquePbr ? matBySpeciesByPartByLod[species][0][lod] : matBySpeciesByPartByLod[species][part][lod]);

				if (idxCount == 0) { ++dbgSkipIdx0; continue; }
				if (!vb) { ++dbgSkipVb0; continue; }
				if (!ib) { ++dbgSkipIb0; continue; }
				if (mat.ptr == 0) { ++dbgSkipMat0; continue; }
				++dbgEligible;

				const int batch = BatchIndex(species, lod, part);

				// Barriers:
				// - indirect args: UAV -> INDIRECT_ARGUMENT
				// - visible index: UAV -> NON_PIXEL_SHADER_RESOURCE (TreeIndirectVS reads this)
				if (m_indirectState[batch] != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
					|| m_visibleState[batch] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
				{
					D3D12_RESOURCE_BARRIER bb[2] = {};
					UINT bi = 0;
					if (m_indirectState[batch] != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
						bb[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(m_indirectArgsDefault[batch].Get(), m_indirectState[batch], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
					if (m_visibleState[batch] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
						bb[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(m_visibleIndexDefault[batch].Get(), m_visibleState[batch], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					if (bi > 0)
						cmd->ResourceBarrier(bi, bb);
					m_indirectState[batch] = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
					m_visibleState[batch] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				}

				ID3D12PipelineState* psoPick = nullptr;
				if (useImposterLod1)
					psoPick = psoImposterLod1->Get();
				else if (part == 1) // 葉のみ alpha-cut（マージ LOD0 part0 は上で不透明 PBR）
					psoPick = (psoLeavesAlphaCut && psoLeavesAlphaCut->IsValid()) ? psoLeavesAlphaCut->Get() : (psoOpaque ? psoOpaque->Get() : nullptr);
				else
					psoPick = psoOpaque ? psoOpaque->Get() : nullptr;
				if (!psoPick)
					continue;

				cmd->SetPipelineState(psoPick);
				cmd->SetGraphicsRootSignature(pbrRootSig->Get());
				cmd->SetGraphicsRootConstantBufferView(0, sceneCbGpu);
				cmd->SetGraphicsRootConstantBufferView(1, materialCbGpu);
				// Root SRVs (space1):
				// - t0: instance data buffer (all instances)
				// - t1: visible index buffer for this (species,lod,part) batch
				cmd->SetGraphicsRootShaderResourceView(2, m_instanceDataDefault->GetGPUVirtualAddress());
				cmd->SetGraphicsRootShaderResourceView(5, m_visibleIndexDefault[batch]->GetGPUVirtualAddress());
				uint32_t treeVisConstants[8];
				if (useImposterLod1)
				{
					TreeVegetation::ImposterBillboardParams ibp{};
					if (!TreeVegetation::GetImposterBillboardParams(&ibp))
					{
						ibp.FootLocal = { 0.f, 0.f, 0.f };
						ibp.HalfWidth = 1.f;
						ibp.Height = 1.f;
					}
					PackTreeVisibleRootConstants(treeVisConstants, 0u, ibp.FootLocal, ibp.HalfWidth, ibp.Height);
				}
				else
					PackTreeVisibleRootConstants(treeVisConstants, 0u, XMFLOAT3(0.f, 0.f, 0.f), 0.f, 0.f);
				cmd->SetGraphicsRoot32BitConstants(6, 8, treeVisConstants, 0);
				cmd->SetGraphicsRootDescriptorTable(3, mat);
				if (iblTable.ptr != 0)
					cmd->SetGraphicsRootDescriptorTable(4, iblTable);

				D3D12_VERTEX_BUFFER_VIEW vbView = vb->View();
				D3D12_INDEX_BUFFER_VIEW ibView = ib->View();
				cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				cmd->IASetVertexBuffers(0, 1, &vbView);
				cmd->IASetIndexBuffer(&ibView);

				if (kTreeDebugBypassExecuteIndirect)
				{
					const uint32_t drawInst = (m_instanceCount < kTreeDebugDirectInstanceCount) ? m_instanceCount : kTreeDebugDirectInstanceCount;
					if (drawInst > 0)
						cmd->DrawIndexedInstanced(idxCount, drawInst, 0, 0, 0);
				}
				else
				{
					cmd->ExecuteIndirect(
						m_cmdSig.Get(),
						1,
						m_indirectArgsDefault[batch].Get(),
						0,
						nullptr,
						0);
				}
				++dbgExecuteCalls;

				// Back to UAV for next frame cull
				if (m_indirectState[batch] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS
					|| m_visibleState[batch] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				{
					D3D12_RESOURCE_BARRIER ab[2] = {};
					UINT ai = 0;
					if (m_indirectState[batch] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
						ab[ai++] = CD3DX12_RESOURCE_BARRIER::Transition(m_indirectArgsDefault[batch].Get(), m_indirectState[batch], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					if (m_visibleState[batch] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
						ab[ai++] = CD3DX12_RESOURCE_BARRIER::Transition(m_visibleIndexDefault[batch].Get(), m_visibleState[batch], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					if (ai > 0)
						cmd->ResourceBarrier(ai, ab);
					m_indirectState[batch] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
					m_visibleState[batch] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				}
			}
		}
	}

	if (dbgExecuteCalls == 0)
	{
		DebugLog("[Trees][Indirect] exec=0 eligible=%u skips(idx=%u vb=%u ib=%u mat=%u) instCount=%u\n",
			dbgEligible, dbgSkipIdx0, dbgSkipVb0, dbgSkipIb0, dbgSkipMat0, m_instanceCount);
	}
	else
	{
		DebugLog("[Trees][Indirect] exec=%u eligible=%u instCount=%u\n",
			dbgExecuteCalls, dbgEligible, m_instanceCount);
	}
	m_lastDrawIndirectBatchCount = dbgExecuteCalls;
}

void TreeGpuCullSystem::DrawIndirect(
	ID3D12GraphicsCommandList* cmd,
	RootSignature* pbrRootSig,
	PipelineState* pbrPso,
	D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
	D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
	D3D12_GPU_DESCRIPTOR_HANDLE treeMaterialTable,
	D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
	VertexBuffer* vb,
	IndexBuffer* ib,
	UINT indexCount)
{
	(void)cmd; (void)pbrRootSig; (void)pbrPso; (void)sceneCbGpu; (void)materialCbGpu;
	(void)treeMaterialTable; (void)iblTable; (void)vb; (void)ib; (void)indexCount;
	// Deprecated: use DrawIndirectLods() (species/lod/part) instead.
}

void TreeGpuCullSystem::DrawDirectInstancedDebug(
	ID3D12GraphicsCommandList* cmd,
	RootSignature* pbrRootSig,
	PipelineState* psoOpaque,
	D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
	D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
	D3D12_GPU_DESCRIPTOR_HANDLE treeMaterialTable,
	D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
	VertexBuffer* vb,
	IndexBuffer* ib,
	UINT indexCount,
	uint32_t instanceCountOverride)
{
	if (!m_valid || !cmd || !pbrRootSig || !pbrRootSig->IsValid() || !psoOpaque || !psoOpaque->IsValid())
		return;
	if (!vb || !ib || indexCount == 0 || treeMaterialTable.ptr == 0)
		return;

	const uint32_t instCount = (instanceCountOverride != 0) ? instanceCountOverride : m_instanceCount;
	if (instCount == 0)
		return;

	cmd->SetPipelineState(psoOpaque->Get());
	cmd->SetGraphicsRootSignature(pbrRootSig->Get());
	cmd->SetGraphicsRootConstantBufferView(0, sceneCbGpu);
	cmd->SetGraphicsRootConstantBufferView(1, materialCbGpu);
	// RootParam[2] = StructuredBuffer<InstanceData> (t0, space1)
	cmd->SetGraphicsRootShaderResourceView(2, m_instanceDataDefault->GetGPUVirtualAddress());
	cmd->SetGraphicsRootDescriptorTable(3, treeMaterialTable);
	if (iblTable.ptr != 0)
		cmd->SetGraphicsRootDescriptorTable(4, iblTable);

	D3D12_VERTEX_BUFFER_VIEW vbView = vb->View();
	D3D12_INDEX_BUFFER_VIEW ibView = ib->View();
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 1, &vbView);
	cmd->IASetIndexBuffer(&ibView);
	cmd->DrawIndexedInstanced(indexCount, instCount, 0, 0, 0);
}

