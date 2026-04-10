#include "ShadowSystem.h"
#include "DescriptorHeap.h"
#include "Engine.h"
#include "SharedStruct.h"
#include "Core/GpuDebugLabels.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// ---- helpers ----

static void ComputeCascadeSplits(float nearClip, float farClip, UINT count, float* outSplits)
{
	constexpr float lambda = 0.75f;
	for (UINT i = 0; i < count; ++i)
	{
		const float p = static_cast<float>(i + 1) / static_cast<float>(count);
		const float logSplit = nearClip * powf(farClip / nearClip, p);
		const float linSplit = nearClip + (farClip - nearClip) * p;
		outSplits[i] = lambda * logSplit + (1.0f - lambda) * linSplit;
	}
}

// Gribb-Hartmann frustum plane extraction for row-vector convention (v_clip = v_world * VP).
// Plane normals point OUTWARD; SphereOutsidePlane tests d > r*|n|.
static void ExtractFrustumPlanes(FXMMATRIX vp, XMFLOAT4 outPlanes[6])
{
	const XMMATRIX vpT = XMMatrixTranspose(vp);
	const XMVECTOR c0 = vpT.r[0];
	const XMVECTOR c1 = vpT.r[1];
	const XMVECTOR c2 = vpT.r[2];
	const XMVECTOR c3 = vpT.r[3];
	XMVECTOR raw[6];
	raw[0] = XMVectorNegate(XMVectorAdd(c3, c0));       // left
	raw[1] = XMVectorNegate(XMVectorSubtract(c3, c0));   // right
	raw[2] = XMVectorNegate(XMVectorAdd(c3, c1));       // bottom
	raw[3] = XMVectorNegate(XMVectorSubtract(c3, c1));   // top
	raw[4] = XMVectorNegate(c2);                         // near (DX12 0-to-1 depth)
	raw[5] = XMVectorNegate(XMVectorSubtract(c3, c2));   // far
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

static XMMATRIX ComputeLightVP(
	const XMMATRIX& cameraView, const XMMATRIX& cameraProj,
	const XMFLOAT3& lightDir, float splitNear, float splitFar,
	float shadowMapSize)
{
	XMMATRIX invViewProj = XMMatrixInverse(nullptr, cameraView * cameraProj);

	const float zNearNdc = 0.0f;
	const float zFarNdc = 1.0f;
	XMVECTOR corners[8];
	int idx = 0;
	for (int z = 0; z < 2; ++z)
	{
		float zn = (z == 0) ? zNearNdc : zFarNdc;
		for (int y = 0; y < 2; ++y)
		{
			for (int x = 0; x < 2; ++x)
			{
				XMVECTOR pt = XMVectorSet(
					x * 2.0f - 1.0f,
					y * 2.0f - 1.0f,
					zn,
					1.0f);
				pt = XMVector4Transform(pt, invViewProj);
				corners[idx++] = XMVectorDivide(pt, XMVectorSplatW(pt));
			}
		}
	}

	XMVECTOR nearCorners[4], farCorners[4];
	for (int i = 0; i < 4; ++i)
	{
		XMVECTOR fullNear = corners[i];
		XMVECTOR fullFar = corners[4 + i];
		XMVECTOR fullDir = fullFar - fullNear;
		float fullLen = XMVectorGetX(XMVector3Length(fullDir));
		if (fullLen < 1e-6f) fullLen = 1.0f;

		XMVECTOR depthNear = XMVector4Transform(fullNear, cameraView);
		float camNearZ = XMVectorGetZ(depthNear);
		XMVECTOR depthFar = XMVector4Transform(fullFar, cameraView);
		float camFarZ = XMVectorGetZ(depthFar);
		float range = camFarZ - camNearZ;
		if (fabsf(range) < 1e-6f) range = 1.0f;

		float tNear = (splitNear - camNearZ) / range;
		float tFar = (splitFar - camNearZ) / range;
		tNear = std::clamp(tNear, 0.0f, 1.0f);
		tFar = std::clamp(tFar, 0.0f, 1.0f);

		nearCorners[i] = fullNear + fullDir * tNear;
		farCorners[i] = fullNear + fullDir * tFar;
	}

	XMVECTOR centre = XMVectorZero();
	for (int i = 0; i < 4; ++i)
		centre += nearCorners[i] + farCorners[i];
	centre /= 8.0f;

	float radius = 0.0f;
	for (int i = 0; i < 4; ++i)
	{
		float d = XMVectorGetX(XMVector3Length(nearCorners[i] - centre));
		radius = std::max(radius, d);
		d = XMVectorGetX(XMVector3Length(farCorners[i] - centre));
		radius = std::max(radius, d);
	}
	radius = ceilf(radius * 16.0f) / 16.0f;

	XMVECTOR lightDirVec = XMVector3Normalize(XMLoadFloat3(&lightDir));
	XMVECTOR lightPos = centre + lightDirVec * radius * 2.0f;

	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetX(XMVector3Dot(lightDirVec, up))) > 0.99f)
		up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, centre, up);
	XMMATRIX lightProj = XMMatrixOrthographicLH(radius * 2.0f, radius * 2.0f, 0.0f, radius * 4.0f);

	XMMATRIX lightVP = lightView * lightProj;
	XMVECTOR shadowOrigin = XMVector4Transform(XMVectorSet(0, 0, 0, 1), lightVP);
	shadowOrigin = shadowOrigin * (shadowMapSize / 2.0f);
	XMVECTOR rounded = XMVectorRound(shadowOrigin);
	XMVECTOR offset = (rounded - shadowOrigin) * (2.0f / shadowMapSize);
	offset = XMVectorSetZ(offset, 0.0f);
	offset = XMVectorSetW(offset, 0.0f);

	lightProj.r[3] += offset;
	return lightView * lightProj;
}

// ---- ShadowSystem implementation ----

bool ShadowSystem::Init(ID3D12Device* device, DescriptorHeap* sceneHeap)
{
	if (!device || !sceneHeap) return false;
	if (!CreateShadowMap(device)) return false;
	if (!CreatePipeline(device)) return false;
	if (!CreateTreeShadowPipeline(device)) return false;
	if (!CreateConstantBuffer(device)) return false;
	if (!CreatePerDrawRing(device)) return false;
	if (!CreateShadowCullPipeline(device))
		printf("ShadowSystem: Shadow cull pipeline init failed (non-fatal, no GPU culling)\n");

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.ArraySize = kCascadeCount;
	srvDesc.Texture2DArray.FirstArraySlice = 0;

	DescriptorHandle* h = sceneHeap->RegisterResource(m_shadowMap.Get(), srvDesc);
	if (!h) return false;
	m_shadowMapSrvGpu = h->HandleGPU;

	m_valid = true;
	return true;
}

void ShadowSystem::Shutdown()
{
	if (m_shadowCB && m_shadowCBMapped)
	{
		m_shadowCB->Unmap(0, nullptr);
		m_shadowCBMapped = nullptr;
	}
	if (m_shadowCullCB && m_shadowCullCBMapped)
	{
		m_shadowCullCB->Unmap(0, nullptr);
		m_shadowCullCBMapped = nullptr;
	}
	m_shadowCB.Reset();
	m_shadowMap.Reset();
	m_dsvHeap.Reset();
	m_rootSignature.Reset();
	m_pso.Reset();
	m_treeShadowRootSig.Reset();
	m_treeShadowPso.Reset();
	m_treeShadowAlphaPso.Reset();
	m_shadowCullRootSig.Reset();
	m_shadowCullPso.Reset();
	m_shadowCmdSig.Reset();
	for (UINT i = 0; i < kCascadeCount; ++i)
	{
		m_shadowVisibleIdx[i].Reset();
		m_shadowIndirectArgs[i].Reset();
	}
	m_shadowArgsResetUpload.Reset();
	m_shadowCullCB.Reset();
	m_shadowCullDescHeap.Reset();
	m_identityVisibleIdx.Reset();
	m_valid = false;
}

bool ShadowSystem::CreateShadowMap(ID3D12Device* device)
{
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_TYPELESS,
		kShadowMapSize, kShadowMapSize,
		kCascadeCount, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

	D3D12_CLEAR_VALUE clearVal = {};
	clearVal.Format = DXGI_FORMAT_D32_FLOAT;
	clearVal.DepthStencil.Depth = 1.0f;

	HRESULT hr = device->CreateCommittedResource(
		&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal,
		IID_PPV_ARGS(m_shadowMap.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) { printf("ShadowSystem: CreateShadowMap failed\n"); return false; }
	GPU_SET_NAME(m_shadowMap.Get(), L"ShadowMap_Tex2DArray");
	m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kCascadeCount;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	hr = device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap));
	if (FAILED(hr)) return false;
	m_dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	for (UINT i = 0; i < kCascadeCount; ++i)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.FirstArraySlice = i;
		dsvDesc.Texture2DArray.ArraySize = 1;
		dsvDesc.Texture2DArray.MipSlice = 0;

		D3D12_CPU_DESCRIPTOR_HANDLE dh = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		dh.ptr += static_cast<SIZE_T>(i) * m_dsvStride;
		device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, dh);
	}
	return true;
}

bool ShadowSystem::CreatePipeline(ID3D12Device* device)
{
	CD3DX12_ROOT_PARAMETER params[1] = {};
	params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 1;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 0;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sig, err;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr))
	{
		if (err) printf("ShadowSystem RS error: %s\n", static_cast<const char*>(err->GetBufferPointer()));
		return false;
	}
	hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> vsBlob;
	hr = D3DReadFileToBlob(L"ShadowPass_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr))
	{
		hr = D3DReadFileToBlob(L"Shaders\\Shadow\\ShadowPass_VS.cso", vsBlob.GetAddressOf());
		if (FAILED(hr)) { printf("ShadowSystem: ShadowPass_VS.cso not found\n"); return false; }
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.InputLayout = Vertex::InputLayout;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	psoDesc.RasterizerState.DepthBias = 1500;
	psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
	psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
	if (FAILED(hr)) { printf("ShadowSystem: PSO create failed\n"); return false; }
	return true;
}

bool ShadowSystem::CreateTreeShadowPipeline(ID3D12Device* device)
{
	// Root signature for tree shadow pass (with visible-index indirection):
	// [0] CBV b0:            LightVP (per-cascade)
	// [1] Root SRV t0 space1: StructuredBuffer<InstanceData>
	// [2] Root SRV t1 space1: StructuredBuffer<uint> visible indices
	// [3] Descriptor table:   t0 space0 (alpha mask texture)
	CD3DX12_ROOT_PARAMETER params[4] = {};
	params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	params[1].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	params[2].InitAsShaderResourceView(1, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	CD3DX12_DESCRIPTOR_RANGE alphaRange;
	alphaRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0); // t0, space0
	params[3].InitAsDescriptorTable(1, &alphaRange, D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_STATIC_SAMPLER_DESC sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 4;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 1;
	rsDesc.pStaticSamplers = &sampler;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sig, err;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr))
	{
		if (err) printf("ShadowSystem TreeRS error: %s\n", static_cast<const char*>(err->GetBufferPointer()));
		return false;
	}
	hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_treeShadowRootSig));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> vsBlob;
	hr = D3DReadFileToBlob(L"TreeShadowPass_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr))
	{
		hr = D3DReadFileToBlob(L"Shaders\\Shadow\\TreeShadowPass_VS.cso", vsBlob.GetAddressOf());
		if (FAILED(hr)) { printf("ShadowSystem: TreeShadowPass_VS.cso not found (non-fatal)\n"); return true; }
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_treeShadowRootSig.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.InputLayout = Vertex::InputLayout;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	psoDesc.RasterizerState.DepthBias = 1500;
	psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_treeShadowPso));
	if (FAILED(hr)) { printf("ShadowSystem: Tree shadow PSO create failed (non-fatal)\n"); return true; }

	ComPtr<ID3DBlob> psBlob;
	hr = D3DReadFileToBlob(L"TreeShadowAlphaTest_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"Shaders\\Shadow\\TreeShadowAlphaTest_PS.cso", psBlob.GetAddressOf());
	if (SUCCEEDED(hr))
	{
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
		device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_treeShadowAlphaPso));
	}

	return true;
}

// ---- Identity buffer for legacy fallback ----

bool ShadowSystem::EnsureIdentityBuffer(ID3D12Device* device, UINT count)
{
	if (m_identityVisibleIdx && m_identitySize >= count) return true;

	m_identitySize = count;
	const UINT64 byteSize = static_cast<UINT64>(count) * sizeof(uint32_t);

	// Create upload + default buffer, fill with 0,1,2,...
	auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto desc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
	HRESULT hr = device->CreateCommittedResource(
		&heapUpload, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(m_identityVisibleIdx.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	uint32_t* p = nullptr;
	m_identityVisibleIdx->Map(0, nullptr, reinterpret_cast<void**>(&p));
	for (UINT i = 0; i < count; ++i) p[i] = i;
	m_identityVisibleIdx->Unmap(0, nullptr);

	return true;
}

// ---- Legacy DrawTreeShadows (fallback when GPU cull unavailable) ----

void ShadowSystem::DrawTreeShadows(
	ID3D12GraphicsCommandList* cmd,
	UINT cascadeIndex,
	ID3D12Resource* instanceDataBuffer,
	UINT instanceCount,
	VertexBuffer* vb,
	IndexBuffer* ib,
	UINT indexCount,
	ID3D12DescriptorHeap* srvHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE alphaMaskSrvGpu)
{
	if (!m_treeShadowPso || !m_treeShadowRootSig || !cmd) return;
	if (!instanceDataBuffer || instanceCount == 0 || !vb || !ib || indexCount == 0) return;
	if (cascadeIndex >= kCascadeCount) return;

	// Ensure identity visible-index buffer for the new VS that expects gVisibleIndex
	{
		ID3D12Device* device = nullptr;
		cmd->GetDevice(IID_PPV_ARGS(&device));
		if (device)
		{
			EnsureIdentityBuffer(device, instanceCount);
			device->Release();
		}
	}

	bool useAlphaTest = (alphaMaskSrvGpu.ptr != 0) && srvHeap && m_treeShadowAlphaPso;
	cmd->SetPipelineState(useAlphaTest ? m_treeShadowAlphaPso.Get() : m_treeShadowPso.Get());
	cmd->SetGraphicsRootSignature(m_treeShadowRootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (useAlphaTest)
	{
		ID3D12DescriptorHeap* heaps[] = { srvHeap };
		cmd->SetDescriptorHeaps(1, heaps);
	}

	D3D12_GPU_VIRTUAL_ADDRESS cbAddr = WritePerDrawCB(m_lightVP[cascadeIndex]);
	if (cbAddr == 0) return;
	cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
	cmd->SetGraphicsRootShaderResourceView(1, instanceDataBuffer->GetGPUVirtualAddress());
	// Slot 2: identity buffer [0,1,2,...] so VS indirection reads gInstanceData[instanceID]
	if (m_identityVisibleIdx)
		cmd->SetGraphicsRootShaderResourceView(2, m_identityVisibleIdx->GetGPUVirtualAddress());
	else
		cmd->SetGraphicsRootShaderResourceView(2, instanceDataBuffer->GetGPUVirtualAddress());

	if (useAlphaTest)
		cmd->SetGraphicsRootDescriptorTable(3, alphaMaskSrvGpu);

	auto vbView = vb->View();
	auto ibView = ib->View();
	cmd->IASetVertexBuffers(0, 1, &vbView);
	cmd->IASetIndexBuffer(&ibView);
	cmd->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
}

// ---- Shadow Cull CS pipeline (Phase B+C) ----

bool ShadowSystem::CreateShadowCullPipeline(ID3D12Device* device)
{
	// Root signature for ShadowFrustumCull_CS:
	// [0] Root CBV b0: ShadowCullCB
	// [1] Descriptor table: t0 SRV (TreeInfo), u0 UAV (VisibleIndex), u1 UAV (IndirectArgs)
	CD3DX12_ROOT_PARAMETER params[2] = {};
	params[0].InitAsConstantBufferView(0);

	CD3DX12_DESCRIPTOR_RANGE ranges[2];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0); // u0, u1
	params[1].InitAsDescriptorTable(2, ranges);

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 2;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> sig, err;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr))
	{
		if (err) printf("ShadowCullRS error: %s\n", static_cast<const char*>(err->GetBufferPointer()));
		return false;
	}
	hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_shadowCullRootSig));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	hr = D3DReadFileToBlob(L"ShadowFrustumCull_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
	{
		hr = D3DReadFileToBlob(L"Shaders\\Shadow\\ShadowFrustumCull_CS.cso", csBlob.GetAddressOf());
		if (FAILED(hr)) { printf("ShadowSystem: ShadowFrustumCull_CS.cso not found\n"); return false; }
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC csPso = {};
	csPso.pRootSignature = m_shadowCullRootSig.Get();
	csPso.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&csPso, IID_PPV_ARGS(&m_shadowCullPso));
	if (FAILED(hr)) { printf("ShadowSystem: Shadow cull PSO failed\n"); return false; }

	// Command signature for ExecuteIndirect (DrawIndexed)
	D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
	argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc = {};
	cmdSigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	cmdSigDesc.NumArgumentDescs = 1;
	cmdSigDesc.pArgumentDescs = &argDesc;
	hr = device->CreateCommandSignature(&cmdSigDesc, nullptr, IID_PPV_ARGS(&m_shadowCmdSig));
	if (FAILED(hr)) { printf("ShadowSystem: Command signature failed\n"); return false; }

	// Cull CB (upload, ring of kCascadeCount slots)
	{
		const UINT64 cbSize = 256ull * kCascadeCount;
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
		hr = device->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_shadowCullCB.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		hr = m_shadowCullCB->Map(0, nullptr, reinterpret_cast<void**>(&m_shadowCullCBMapped));
		if (FAILED(hr)) return false;
		memset(m_shadowCullCBMapped, 0, static_cast<size_t>(cbSize));
	}

	return true;
}

bool ShadowSystem::CreateShadowCullBuffers(ID3D12Device* device, UINT maxInstances)
{
	if (m_shadowCullMaxInstances >= maxInstances && m_shadowVisibleIdx[0])
		return true; // already large enough

	m_shadowCullMaxInstances = maxInstances;

	auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	HRESULT hr;

	for (UINT c = 0; c < kCascadeCount; ++c)
	{
		// Visible index buffer: uint32[maxInstances], UAV + SRV
		{
			auto desc = CD3DX12_RESOURCE_DESC::Buffer(
				static_cast<UINT64>(maxInstances) * sizeof(uint32_t),
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			hr = device->CreateCommittedResource(
				&heapDefault, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
				IID_PPV_ARGS(m_shadowVisibleIdx[c].ReleaseAndGetAddressOf()));
			if (FAILED(hr)) return false;
		}

		m_shadowVisIdxState[c] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

		// Indirect args buffer: D3D12_DRAW_INDEXED_ARGUMENTS (20 bytes), UAV
		{
			auto desc = CD3DX12_RESOURCE_DESC::Buffer(
				sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			hr = device->CreateCommittedResource(
				&heapDefault, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
				IID_PPV_ARGS(m_shadowIndirectArgs[c].ReleaseAndGetAddressOf()));
			if (FAILED(hr)) return false;
			m_shadowArgsState[c] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		}
	}

	// Reset template: zeroed D3D12_DRAW_INDEXED_ARGUMENTS (upload)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
		hr = device->CreateCommittedResource(
			&heapUpload, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_shadowArgsResetUpload.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		void* p = nullptr;
		m_shadowArgsResetUpload->Map(0, nullptr, &p);
		memset(p, 0, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
		m_shadowArgsResetUpload->Unmap(0, nullptr);
	}

	// Shader-visible descriptor heap for cull CS.
	// Per cascade: [SRV:TreeInfo, UAV:VisibleIdx, UAV:IndirectArgs] = 3 descriptors
	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.NumDescriptors = kCascadeCount * 3;
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_shadowCullDescHeap.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		m_shadowCullDescStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	// Descriptors are created per-cascade in DispatchShadowCull (TreeInfo resource not known here)

	return true;
}

// Layout of ShadowCullCB (must match HLSL):
struct ShadowCullCBData
{
	XMFLOAT4 FrustumPlanes[6]; // 96 bytes
	XMFLOAT4 Params;           // x: instanceCount, y: maxOutput
	XMFLOAT4 CameraPos;        // xyz: camera world, w: maxShadowDistance (XZ metres)
};

void ShadowSystem::DispatchShadowCull(
	ID3D12GraphicsCommandList* cmd,
	UINT cascadeIndex,
	ID3D12Resource* treeInfoBuffer,
	UINT instanceCount,
	UINT maxOutput,
	const XMFLOAT3& cameraPos,
	float maxShadowDist)
{
	if (!m_shadowCullPso || !m_shadowCullRootSig || !cmd || !treeInfoBuffer) return;
	if (cascadeIndex >= kCascadeCount || instanceCount == 0 || maxOutput == 0) return;

	ID3D12Device* device = nullptr;
	cmd->GetDevice(IID_PPV_ARGS(&device));
	if (!device) return;

	// Lazy buffer creation/resize
	if (!m_shadowVisibleIdx[0] || m_shadowCullMaxInstances < instanceCount)
	{
		if (!CreateShadowCullBuffers(device, instanceCount))
		{
			device->Release();
			return;
		}
	}

	// Build descriptors once (reset flag when buffers are newly created)
	static bool s_descriptorsBuilt[kCascadeCount] = {};
	static ID3D12Resource* s_lastTreeInfoBuf = nullptr;
	static UINT s_lastMaxInstances = 0;
	if (m_shadowCullMaxInstances != s_lastMaxInstances)
	{
		for (UINT i = 0; i < kCascadeCount; ++i) s_descriptorsBuilt[i] = false;
		s_lastMaxInstances = m_shadowCullMaxInstances;
	}
	const bool needRebuild = !s_descriptorsBuilt[cascadeIndex]
		|| s_lastTreeInfoBuf != treeInfoBuffer;
	if (needRebuild)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = m_shadowCullDescHeap->GetCPUDescriptorHandleForHeapStart();
		SIZE_T offset = static_cast<SIZE_T>(cascadeIndex) * 3 * m_shadowCullDescStride;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.NumElements = instanceCount;
		srvDesc.Buffer.StructureByteStride = 112;
		D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = { cpuBase.ptr + offset };
		device->CreateShaderResourceView(treeInfoBuffer, &srvDesc, srvHandle);

		D3D12_UNORDERED_ACCESS_VIEW_DESC visUavDesc = {};
		visUavDesc.Format = DXGI_FORMAT_UNKNOWN;
		visUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		visUavDesc.Buffer.NumElements = instanceCount;
		visUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
		D3D12_CPU_DESCRIPTOR_HANDLE visHandle = { cpuBase.ptr + offset + m_shadowCullDescStride };
		device->CreateUnorderedAccessView(m_shadowVisibleIdx[cascadeIndex].Get(), nullptr, &visUavDesc, visHandle);

		D3D12_UNORDERED_ACCESS_VIEW_DESC argsUavDesc = {};
		argsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		argsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		argsUavDesc.Buffer.NumElements = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / sizeof(uint32_t);
		argsUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		D3D12_CPU_DESCRIPTOR_HANDLE argsHandle = { cpuBase.ptr + offset + 2 * m_shadowCullDescStride };
		device->CreateUnorderedAccessView(m_shadowIndirectArgs[cascadeIndex].Get(), nullptr, &argsUavDesc, argsHandle);

		s_descriptorsBuilt[cascadeIndex] = true;
		if (cascadeIndex == 0) s_lastTreeInfoBuf = treeInfoBuffer;
	}
	device->Release();

	// Transition both resources to their pre-dispatch states
	{
		D3D12_RESOURCE_BARRIER bb[2];
		UINT count = 0;
		if (m_shadowArgsState[cascadeIndex] != D3D12_RESOURCE_STATE_COPY_DEST)
		{
			bb[count++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_shadowIndirectArgs[cascadeIndex].Get(),
				m_shadowArgsState[cascadeIndex], D3D12_RESOURCE_STATE_COPY_DEST);
		}
		if (m_shadowVisIdxState[cascadeIndex] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS
			&& m_shadowVisIdxState[cascadeIndex] != static_cast<D3D12_RESOURCE_STATES>(0))
		{
			bb[count++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_shadowVisibleIdx[cascadeIndex].Get(),
				m_shadowVisIdxState[cascadeIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
		if (count > 0) cmd->ResourceBarrier(count, bb);
		m_shadowVisIdxState[cascadeIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	// Reset indirect args to zero (InstanceCount=0, IndexCount pre-set)
	cmd->CopyBufferRegion(
		m_shadowIndirectArgs[cascadeIndex].Get(), 0,
		m_shadowArgsResetUpload.Get(), 0,
		sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
	{
		auto b = CD3DX12_RESOURCE_BARRIER::Transition(
			m_shadowIndirectArgs[cascadeIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b);
		m_shadowArgsState[cascadeIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	// Write cull CB for this cascade
	{
		ShadowCullCBData cb = {};
		ExtractFrustumPlanes(m_lightVP[cascadeIndex], cb.FrustumPlanes);
		cb.Params = XMFLOAT4(static_cast<float>(instanceCount), static_cast<float>(maxOutput), 1.0f, 0);
		cb.CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, maxShadowDist);
		memcpy(m_shadowCullCBMapped + 256ull * cascadeIndex, &cb, sizeof(cb));
	}

	// Dispatch
	cmd->SetComputeRootSignature(m_shadowCullRootSig.Get());
	cmd->SetPipelineState(m_shadowCullPso.Get());

	ID3D12DescriptorHeap* heaps[] = { m_shadowCullDescHeap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);

	cmd->SetComputeRootConstantBufferView(0,
		m_shadowCullCB->GetGPUVirtualAddress() + 256ull * cascadeIndex);

	D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_shadowCullDescHeap->GetGPUDescriptorHandleForHeapStart();
	gpuBase.ptr += static_cast<SIZE_T>(cascadeIndex) * 3 * m_shadowCullDescStride;
	cmd->SetComputeRootDescriptorTable(1, gpuBase);

	const UINT threadGroups = (instanceCount + 63) / 64;
	cmd->Dispatch(threadGroups, 1, 1);

	// UAV barriers so draw can read results
	D3D12_RESOURCE_BARRIER barriers[2];
	barriers[0] = CD3DX12_RESOURCE_BARRIER::UAV(m_shadowVisibleIdx[cascadeIndex].Get());
	barriers[1] = CD3DX12_RESOURCE_BARRIER::UAV(m_shadowIndirectArgs[cascadeIndex].Get());
	cmd->ResourceBarrier(2, barriers);
}

void ShadowSystem::SetShadowIndexCount(UINT cascadeIndex, UINT indexCount)
{
	if (cascadeIndex >= kCascadeCount || !m_shadowArgsResetUpload) return;
	// Write IndexCountPerInstance into the reset template so DispatchShadowCull
	// copies it along with zeroed InstanceCount.
	void* p = nullptr;
	m_shadowArgsResetUpload->Map(0, nullptr, &p);
	auto* args = static_cast<D3D12_DRAW_INDEXED_ARGUMENTS*>(p);
	args->IndexCountPerInstance = indexCount;
	args->InstanceCount = 0;
	args->StartIndexLocation = 0;
	args->BaseVertexLocation = 0;
	args->StartInstanceLocation = 0;
	m_shadowArgsResetUpload->Unmap(0, nullptr);
}

void ShadowSystem::DrawTreeShadowsIndirect(
	ID3D12GraphicsCommandList* cmd,
	UINT cascadeIndex,
	ID3D12Resource* instanceDataBuffer,
	VertexBuffer* vb,
	IndexBuffer* ib,
	ID3D12DescriptorHeap* srvHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE alphaMaskSrvGpu)
{
	if (!m_treeShadowPso || !m_treeShadowRootSig || !m_shadowCmdSig || !cmd) return;
	if (!instanceDataBuffer || !vb || !ib) return;
	if (cascadeIndex >= kCascadeCount) return;
	if (!m_shadowVisibleIdx[cascadeIndex] || !m_shadowIndirectArgs[cascadeIndex]) return;

	// Transition: args UAV → INDIRECT_ARGUMENT, visible idx UAV → NON_PIXEL_SHADER_RESOURCE
	{
		D3D12_RESOURCE_BARRIER bb[2];
		UINT count = 0;
		if (m_shadowArgsState[cascadeIndex] != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
		{
			bb[count++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_shadowIndirectArgs[cascadeIndex].Get(),
				m_shadowArgsState[cascadeIndex], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			m_shadowArgsState[cascadeIndex] = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		}
		if (m_shadowVisIdxState[cascadeIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
		{
			bb[count++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_shadowVisibleIdx[cascadeIndex].Get(),
				m_shadowVisIdxState[cascadeIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			m_shadowVisIdxState[cascadeIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		}
		if (count > 0) cmd->ResourceBarrier(count, bb);
	}

	bool useAlphaTest = (alphaMaskSrvGpu.ptr != 0) && srvHeap && m_treeShadowAlphaPso;
	cmd->SetPipelineState(useAlphaTest ? m_treeShadowAlphaPso.Get() : m_treeShadowPso.Get());
	cmd->SetGraphicsRootSignature(m_treeShadowRootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (useAlphaTest)
	{
		ID3D12DescriptorHeap* heaps[] = { srvHeap };
		cmd->SetDescriptorHeaps(1, heaps);
	}

	D3D12_GPU_VIRTUAL_ADDRESS cbAddr = WritePerDrawCB(m_lightVP[cascadeIndex]);
	if (cbAddr == 0) return;
	cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
	cmd->SetGraphicsRootShaderResourceView(1, instanceDataBuffer->GetGPUVirtualAddress());
	cmd->SetGraphicsRootShaderResourceView(2, m_shadowVisibleIdx[cascadeIndex]->GetGPUVirtualAddress());

	if (useAlphaTest)
		cmd->SetGraphicsRootDescriptorTable(3, alphaMaskSrvGpu);

	auto vbView = vb->View();
	auto ibView = ib->View();
	cmd->IASetVertexBuffers(0, 1, &vbView);
	cmd->IASetIndexBuffer(&ibView);

	cmd->ExecuteIndirect(
		m_shadowCmdSig.Get(),
		1,
		m_shadowIndirectArgs[cascadeIndex].Get(),
		0,
		nullptr, 0);
}

// ---- Existing methods (unchanged) ----

bool ShadowSystem::CreateConstantBuffer(ID3D12Device* device)
{
	const UINT64 sz = sizeof(ShadowConstants);
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(sz);

	HRESULT hr = device->CreateCommittedResource(
		&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(m_shadowCB.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	hr = m_shadowCB->Map(0, nullptr, reinterpret_cast<void**>(&m_shadowCBMapped));
	if (FAILED(hr)) return false;

	memset(m_shadowCBMapped, 0, sizeof(ShadowConstants));
	return true;
}

void ShadowSystem::UpdateCascades(
	const XMMATRIX& cameraView, const XMMATRIX& cameraProj,
	const XMFLOAT3& lightDir, float nearClip, float farClip)
{
	ComputeCascadeSplits(nearClip, farClip, kCascadeCount, m_cascadeSplits);

	for (UINT i = 0; i < kCascadeCount; ++i)
	{
		float splitNear = (i == 0) ? nearClip : m_cascadeSplits[i - 1];
		float splitFar = m_cascadeSplits[i];
		m_lightVP[i] = ComputeLightVP(
			cameraView, cameraProj, lightDir,
			splitNear, splitFar,
			static_cast<float>(kShadowMapSize));
	}

	if (m_shadowCBMapped)
	{
		for (UINT i = 0; i < kCascadeCount; ++i)
			m_shadowCBMapped->LightVP[i] = XMMatrixTranspose(m_lightVP[i]);
		m_shadowCBMapped->CascadeSplits = XMFLOAT4(
			m_cascadeSplits[0], m_cascadeSplits[1], m_cascadeSplits[2], 0.0f);
	}
}

void ShadowSystem::BeginShadowPass(ID3D12GraphicsCommandList* cmd, UINT cascadeIndex)
{
	if (cascadeIndex >= kCascadeCount) return;

	if (m_currentState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_shadowMap.Get(), m_currentState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		cmd->ResourceBarrier(1, &barrier);
		m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	dsv.ptr += static_cast<SIZE_T>(cascadeIndex) * m_dsvStride;
	cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

	D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0, 1 };
	D3D12_RECT sr = { 0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize) };
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &sr);
}

void ShadowSystem::EndShadowPass(ID3D12GraphicsCommandList* /*cmd*/, UINT /*cascadeIndex*/)
{
}

void ShadowSystem::TransitionToSRV(ID3D12GraphicsCommandList* cmd)
{
	if (m_currentState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		m_shadowMap.Get(), m_currentState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &barrier);
	m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

XMMATRIX ShadowSystem::GetLightVPTransposed(UINT cascade) const
{
	if (cascade >= kCascadeCount) return XMMatrixIdentity();
	return XMMatrixTranspose(m_lightVP[cascade]);
}

D3D12_GPU_VIRTUAL_ADDRESS ShadowSystem::GetShadowCBAddress() const
{
	return m_shadowCB ? m_shadowCB->GetGPUVirtualAddress() : 0;
}

bool ShadowSystem::CreatePerDrawRing(ID3D12Device* device)
{
	const UINT64 sz = 256ull * kPerDrawRingSlots;
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(sz);
	HRESULT hr = device->CreateCommittedResource(
		&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(m_perDrawRing.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;
	hr = m_perDrawRing->Map(0, nullptr, reinterpret_cast<void**>(&m_perDrawRingMapped));
	return SUCCEEDED(hr);
}

D3D12_GPU_VIRTUAL_ADDRESS ShadowSystem::WritePerDrawCB(const XMMATRIX& worldLightVP)
{
	if (!m_perDrawRingMapped || m_perDrawRingOffset >= kPerDrawRingSlots)
		return 0;

	const UINT slot = m_perDrawRingOffset++;
	const UINT64 byteOffset = 256ull * slot;

	XMMATRIX transposed = XMMatrixTranspose(worldLightVP);
	memcpy(m_perDrawRingMapped + byteOffset, &transposed, sizeof(XMMATRIX));

	return m_perDrawRing->GetGPUVirtualAddress() + byteOffset;
}
