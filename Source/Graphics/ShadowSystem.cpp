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
	constexpr float lambda = 0.70f; // PSSM blend (log vs uniform); 0.7 suits open-world forests
	for (UINT i = 0; i < count; ++i)
	{
		const float p = static_cast<float>(i + 1) / static_cast<float>(count);
		const float logSplit = nearClip * powf(farClip / nearClip, p);
		const float linSplit = nearClip + (farClip - nearClip) * p;
		outSplits[i] = lambda * logSplit + (1.0f - lambda) * linSplit;
	}
}

static XMMATRIX ComputeLightVP(
	const XMMATRIX& cameraView, const XMMATRIX& cameraProj,
	const XMFLOAT3& lightDir, float splitNear, float splitFar,
	float shadowMapSize)
{
	XMMATRIX invViewProj = XMMatrixInverse(nullptr, cameraView * cameraProj);

	// NDC frustum corners for the cascade slice
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

	// Interpolate near/far slices in view depth
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

	// Compute centre and radius for a bounding sphere
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
	XMVECTOR lightPos = centre - lightDirVec * radius * 2.0f;

	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetX(XMVector3Dot(lightDirVec, up))) > 0.99f)
		up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, centre, up);
	XMMATRIX lightProj = XMMatrixOrthographicLH(radius * 2.0f, radius * 2.0f, 0.0f, radius * 4.0f);

	// Snap to texel grid to reduce shimmer
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

	// Register shadow map array as SRV in the scene descriptor heap
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
	m_shadowCB.Reset();
	m_shadowMap.Reset();
	m_dsvHeap.Reset();
	m_rootSignature.Reset();
	m_pso.Reset();
	m_treeShadowRootSig.Reset();
	m_treeShadowPso.Reset();
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

	// DSV heap: one DSV per cascade slice
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

		D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(i) * m_dsvStride;
		device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, h);
	}
	return true;
}

bool ShadowSystem::CreatePipeline(ID3D12Device* device)
{
	// Root signature for shadow pass: single root CBV b0 (WorldLightVP per draw)
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

	// PSO: depth-only, VS only (no PS), using shared Vertex layout
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
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; // front-face culling reduces peter-panning
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
	// Root signature for tree shadow pass:
	// [0] CBV b0: LightVP (per-cascade matrix)
	// [1] Root SRV t0 space1: StructuredBuffer<InstanceData>
	CD3DX12_ROOT_PARAMETER params[2] = {};
	params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	params[1].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX); // t0, space1

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 2;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 0;
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
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // trees have two-sided foliage
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
	return true;
}

void ShadowSystem::DrawTreeShadows(
	ID3D12GraphicsCommandList* cmd,
	UINT cascadeIndex,
	ID3D12Resource* instanceDataBuffer,
	UINT instanceCount,
	VertexBuffer* vb,
	IndexBuffer* ib,
	UINT indexCount)
{
	if (!m_treeShadowPso || !m_treeShadowRootSig || !cmd) return;
	if (!instanceDataBuffer || instanceCount == 0 || !vb || !ib || indexCount == 0) return;
	if (cascadeIndex >= kCascadeCount) return;

	cmd->SetPipelineState(m_treeShadowPso.Get());
	cmd->SetGraphicsRootSignature(m_treeShadowRootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Bind LightVP for this cascade via per-draw ring
	XMMATRIX lightVP = XMMatrixTranspose(m_lightVP[cascadeIndex]);
	lightVP = XMMatrixTranspose(lightVP);  // back to row-major for WritePerDrawCB
	D3D12_GPU_VIRTUAL_ADDRESS cbAddr = WritePerDrawCB(m_lightVP[cascadeIndex]);
	if (cbAddr == 0) return;
	cmd->SetGraphicsRootConstantBufferView(0, cbAddr);

	// Bind instance data SRV
	cmd->SetGraphicsRootShaderResourceView(1, instanceDataBuffer->GetGPUVirtualAddress());

	auto vbView = vb->View();
	auto ibView = ib->View();
	cmd->IASetVertexBuffers(0, 1, &vbView);
	cmd->IASetIndexBuffer(&ibView);
	cmd->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
}

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
			m_cascadeSplits[0], m_cascadeSplits[1],
			m_cascadeSplits[2],
			(kCascadeCount >= 4) ? m_cascadeSplits[3] : 0.0f);
	}
}

void ShadowSystem::BeginShadowPass(ID3D12GraphicsCommandList* cmd, UINT cascadeIndex)
{
	if (cascadeIndex >= kCascadeCount) return;

	// Ensure resource is in DSV state
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
	// Intentionally empty; barriers batched in TransitionToSRV.
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
