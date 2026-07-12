#include "AtmosphereSystem.h"
#include "ShadowSystem.h"
#include "DescriptorHeap.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

static bool LoadBlob(const wchar_t* name, ID3DBlob** out)
{
	if (SUCCEEDED(D3DReadFileToBlob(name, out))) return true;
	wchar_t alt[256];
	swprintf_s(alt, L"Shaders\\Atmospheric\\%s", name);
	return SUCCEEDED(D3DReadFileToBlob(alt, out));
}

bool AtmosphereSystem::Init(ID3D12Device* device, DescriptorHeap* sceneHeap,
	UINT fullWidth, UINT fullHeight, ID3D12Resource* sceneDepthResource)
{
	if (!device || !sceneHeap || fullWidth == 0 || fullHeight == 0) return false;
	m_fullW = fullWidth;
	m_fullH = fullHeight;
	m_quarterW = (fullWidth + 3) / 4;
	m_quarterH = (fullHeight + 3) / 4;

	if (!CreateVolumetricResources(device, sceneHeap)) return false;
	if (!CreateVolumetricPipeline(device)) return false;
	if (!CreateTemporalPipeline(device)) return false;
	if (!CreateCompositePipeline(device, sceneHeap)) return false;
	if (!CreateConstantBuffer(device)) return false;

	m_valid = true;
	return true;
}

void AtmosphereSystem::Shutdown()
{
	if (m_cb && m_cbMapped) { m_cb->Unmap(0, nullptr); m_cbMapped = nullptr; }
	if (m_temporalCB && m_temporalCBMapped) { m_temporalCB->Unmap(0, nullptr); m_temporalCBMapped = nullptr; }
	m_cb.Reset();
	m_temporalCB.Reset();
	m_volumetricTex.Reset();
	m_temporalTexA.Reset();
	m_temporalTexB.Reset();
	m_volDescHeap.Reset();
	m_volRootSig.Reset();
	m_volPso.Reset();
	m_temporalDescHeap.Reset();
	m_temporalRootSig.Reset();
	m_temporalPso.Reset();
	m_compDescHeap.Reset();
	m_compRootSig.Reset();
	m_compPso.Reset();
	m_valid = false;
}

static ComPtr<ID3D12Resource> CreateQuarterTexture(ID3D12Device* device, UINT w, UINT h, const wchar_t* name)
{
	auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto rd = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		w, h, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	ComPtr<ID3D12Resource> tex;
	HRESULT hr = device->CreateCommittedResource(
		&hp, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(tex.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return nullptr;
	GPU_SET_NAME(tex.Get(), name);
	return tex;
}

bool AtmosphereSystem::CreateVolumetricResources(ID3D12Device* device, DescriptorHeap* sceneHeap)
{
	m_volumetricTex = CreateQuarterTexture(device, m_quarterW, m_quarterH, L"VolumetricTex_Current");
	if (!m_volumetricTex) return false;

	m_temporalTexA = CreateQuarterTexture(device, m_quarterW, m_quarterH, L"TemporalTex_A");
	m_temporalTexB = CreateQuarterTexture(device, m_quarterW, m_quarterH, L"TemporalTex_B");
	if (!m_temporalTexA || !m_temporalTexB) return false;
	m_temporalPingPong = false;

	// CS-internal descriptor heap: SRV depth(0), SRV shadow(1), UAV volumetric(2)
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = 3;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_volDescHeap));
	if (FAILED(hr)) return false;
	m_volDescStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = m_volDescHeap->GetCPUDescriptorHandleForHeapStart();
	uavCpu.ptr += 2 * m_volDescStride;
	device->CreateUnorderedAccessView(m_volumetricTex.Get(), nullptr, &uavDesc, uavCpu);

	// Temporal descriptor heap: [0]=depth SRV, [1]=current SRV, [2]=prev SRV, [3]=output UAV
	D3D12_DESCRIPTOR_HEAP_DESC tempHeapDesc = {};
	tempHeapDesc.NumDescriptors = 4;
	tempHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	tempHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = device->CreateDescriptorHeap(&tempHeapDesc, IID_PPV_ARGS(&m_temporalDescHeap));
	if (FAILED(hr)) return false;
	m_temporalDescStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Composite descriptor heap: [0]=depth, [1]=volumetric (temporal output)
	D3D12_DESCRIPTOR_HEAP_DESC compHeapDesc = {};
	compHeapDesc.NumDescriptors = 2;
	compHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	compHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = device->CreateDescriptorHeap(&compHeapDesc, IID_PPV_ARGS(&m_compDescHeap));
	if (FAILED(hr)) return false;
	m_compDescStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	return true;
}

bool AtmosphereSystem::CreateVolumetricPipeline(ID3D12Device* device)
{
	CD3DX12_ROOT_PARAMETER params[4] = {};
	params[0].InitAsConstantBufferView(0);
	params[1].InitAsConstantBufferView(1);
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
	params[2].InitAsDescriptorTable(1, &srvRange);
	CD3DX12_DESCRIPTOR_RANGE uavRange;
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	params[3].InitAsDescriptorTable(1, &uavRange);

	D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
	samplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	samplers[0].ShaderRegister = 0;
	samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_POINT);
	samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 4;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 2;
	rsDesc.pStaticSamplers = samplers;

	ComPtr<ID3DBlob> sig, err;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr)) { if (err) printf("AtmVol RS: %s\n", (char*)err->GetBufferPointer()); return false; }
	hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_volRootSig));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	if (!LoadBlob(L"Volumetric_CS.cso", csBlob.GetAddressOf()))
	{ printf("AtmosphereSystem: Volumetric_CS.cso not found\n"); return false; }

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_volRootSig.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_volPso));
	if (FAILED(hr)) { printf("AtmosphereSystem: Vol PSO failed\n"); return false; }
	return true;
}

bool AtmosphereSystem::CreateTemporalPipeline(ID3D12Device* device)
{
	// Root: CBV b0, SRV table (t0=depth, t1=current, t2=prev), UAV table (u0=output)
	CD3DX12_ROOT_PARAMETER params[3] = {};
	params[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // t0, t1, t2
	params[1].InitAsDescriptorTable(1, &srvRange);
	CD3DX12_DESCRIPTOR_RANGE uavRange;
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	params[2].InitAsDescriptorTable(1, &uavRange);

	D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
	samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
	samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_POINT);
	samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 2;
	rsDesc.pStaticSamplers = samplers;

	ComPtr<ID3DBlob> sig, err;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr)) { if (err) printf("Temporal RS: %s\n", (char*)err->GetBufferPointer()); return false; }
	hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_temporalRootSig));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	if (!LoadBlob(L"Temporal_CS.cso", csBlob.GetAddressOf()))
	{ printf("AtmosphereSystem: Temporal_CS.cso not found\n"); return false; }

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_temporalRootSig.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_temporalPso));
	if (FAILED(hr)) { printf("AtmosphereSystem: Temporal PSO failed\n"); return false; }
	return true;
}

bool AtmosphereSystem::CreateCompositePipeline(ID3D12Device* device, DescriptorHeap* /*sceneHeap*/)
{
	CD3DX12_ROOT_PARAMETER params[2] = {};
	params[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // t0=depth, t1=volumetric
	params[1].InitAsDescriptorTable(1, &srvRange);

	D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
	// s0: point sampler for depth and bilateral
	samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
	samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	// s1: linear sampler for fallback / smooth sampling
	samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
	samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 2;
	rsDesc.pParameters = params;
	rsDesc.NumStaticSamplers = 2;
	rsDesc.pStaticSamplers = samplers;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sig, err;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_compRootSig));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> vsBlob, psBlob;
	if (!SUCCEEDED(D3DReadFileToBlob(L"ToneMap_VS.cso", vsBlob.GetAddressOf())))
	{
		if (!SUCCEEDED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", vsBlob.GetAddressOf())))
			return false;
	}
	if (!LoadBlob(L"FogComposite_PS.cso", psBlob.GetAddressOf()))
	{ printf("AtmosphereSystem: FogComposite_PS.cso not found\n"); return false; }

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_compRootSig.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	auto& rt0 = psoDesc.BlendState.RenderTarget[0];
	rt0.BlendEnable = TRUE;
	rt0.SrcBlend = D3D12_BLEND_ONE;
	rt0.DestBlend = D3D12_BLEND_SRC_ALPHA;
	rt0.BlendOp = D3D12_BLEND_OP_ADD;
	rt0.SrcBlendAlpha = D3D12_BLEND_ZERO;
	rt0.DestBlendAlpha = D3D12_BLEND_ONE;
	rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDesc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_compPso));
	if (FAILED(hr)) { printf("AtmosphereSystem: Composite PSO failed\n"); return false; }
	return true;
}

bool AtmosphereSystem::CreateConstantBuffer(ID3D12Device* device)
{
	auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	// Atmosphere CB
	{
		auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(AtmosphereCBData));
		HRESULT hr = device->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_cb.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		hr = m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped));
		if (FAILED(hr)) return false;
		memset(m_cbMapped, 0, sizeof(AtmosphereCBData));
	}

	// Temporal CB
	{
		auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(TemporalCBData));
		HRESULT hr = device->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_temporalCB.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		hr = m_temporalCB->Map(0, nullptr, reinterpret_cast<void**>(&m_temporalCBMapped));
		if (FAILED(hr)) return false;
		memset(m_temporalCBMapped, 0, sizeof(TemporalCBData));
	}

	return true;
}

void AtmosphereSystem::Execute(
	ID3D12GraphicsCommandList* cmd,
	ID3D12DescriptorHeap* sceneDescriptorHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE hdrSceneSrvGpu,
	D3D12_CPU_DESCRIPTOR_HANDLE hdrSceneRtvCpu,
	ID3D12Resource* hdrResource,
	ID3D12Resource* depthResource,
	const ShadowSystem* shadow,
	const XMMATRIX& invViewProj,
	const XMFLOAT3& cameraPos,
	const XMFLOAT3& sunDir,
	const XMFLOAT4& sunColor,
	UINT frameIndex,
	const AtmosphereParams& params)
{
	if (!m_valid || !cmd) return;
	if (!params.enableFog && !params.enableVolumetric) return;

	// Current ViewProj for temporal bookkeeping
	XMMATRIX curViewProj = XMMatrixInverse(nullptr, invViewProj);

	// Update CB
	if (m_cbMapped)
	{
		m_cbMapped->InvViewProj = XMMatrixTranspose(invViewProj);
		m_cbMapped->CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 1.0f);
		m_cbMapped->SunDirection = XMFLOAT4(sunDir.x, sunDir.y, sunDir.z, params.fogMaxOpacity); // .w=FogMaxOpacity
		m_cbMapped->SunColor = XMFLOAT4(sunColor.x, sunColor.y, sunColor.z, params.noiseStrength);
		m_cbMapped->FogParams = XMFLOAT4(params.fogDensity, params.scatteringG,
			params.heightFalloff, params.baseHeight);
		const bool volWillRun = params.enableVolumetric && shadow && shadow->IsValid();
		m_cbMapped->FrameParams = XMFLOAT4(
			static_cast<float>(frameIndex),
			static_cast<float>(m_quarterW),
			static_cast<float>(m_quarterH),
			volWillRun ? 1.0f : 0.0f);
		m_cbMapped->FogColor = XMFLOAT4(params.fogColorR, params.fogColorG, params.fogColorB, params.maxFogDistance);
	}

	D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_cb ? m_cb->GetGPUVirtualAddress() : 0;
	D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddr = shadow ? shadow->GetShadowCBAddress() : 0;

	const bool volCsRan = params.enableVolumetric && shadow && shadow->IsValid();
	const bool doTemporal = volCsRan && params.enableTemporal && m_hasPrevViewProj && m_temporalPso;

	// Determine ping-pong targets
	ID3D12Resource* prevTex = m_temporalPingPong ? m_temporalTexB.Get() : m_temporalTexA.Get();
	ID3D12Resource* outTex  = m_temporalPingPong ? m_temporalTexA.Get() : m_temporalTexB.Get();

	// ---- Volumetric CS (quarter-res) ----
	if (volCsRan)
	{
		GPU_CMD_BEGIN_EVENT(cmd, 100, 140, 200, L"Atmosphere: Volumetric CS (1/4 res)");

		auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			depthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		cmd->ResourceBarrier(1, &depthBarrier);

		auto* dev = g_Engine->Device();
		D3D12_CPU_DESCRIPTOR_HANDLE volCpuBase = m_volDescHeap->GetCPUDescriptorHandleForHeapStart();

		D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
		depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		depthSrvDesc.Texture2D.MipLevels = 1;
		dev->CreateShaderResourceView(depthResource, &depthSrvDesc, volCpuBase);

		D3D12_CPU_DESCRIPTOR_HANDLE shadowSlot = volCpuBase;
		shadowSlot.ptr += m_volDescStride;
		D3D12_SHADER_RESOURCE_VIEW_DESC smSrv = {};
		smSrv.Format = DXGI_FORMAT_R32_FLOAT;
		smSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		smSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		smSrv.Texture2DArray.MipLevels = 1;
		smSrv.Texture2DArray.ArraySize = ShadowSystem::kCascadeCount;
		dev->CreateShaderResourceView(shadow->GetShadowMapResource(), &smSrv, shadowSlot);

		ID3D12DescriptorHeap* heaps[] = { m_volDescHeap.Get() };
		cmd->SetDescriptorHeaps(1, heaps);
		cmd->SetComputeRootSignature(m_volRootSig.Get());
		cmd->SetPipelineState(m_volPso.Get());

		cmd->SetComputeRootConstantBufferView(0, cbAddr);
		cmd->SetComputeRootConstantBufferView(1, shadowCBAddr);
		D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_volDescHeap->GetGPUDescriptorHandleForHeapStart();
		cmd->SetComputeRootDescriptorTable(2, gpuBase);
		D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = gpuBase;
		uavGpu.ptr += 2 * m_volDescStride;
		cmd->SetComputeRootDescriptorTable(3, uavGpu);

		UINT groupsX = (m_quarterW + 7) / 8;
		UINT groupsY = (m_quarterH + 7) / 8;
		cmd->Dispatch(groupsX, groupsY, 1);

		GPU_CMD_END_EVENT(cmd);

		// ---- Temporal Reprojection CS ----
		if (doTemporal)
		{
			GPU_CMD_BEGIN_EVENT(cmd, 110, 150, 210, L"Atmosphere: Temporal Reprojection CS");

			// Transitions: volumetricTex UAV→SRV, prevTex UAV→SRV, outTex stays UAV
			D3D12_RESOURCE_BARRIER preTemporalBarriers[2] = {};
			preTemporalBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_volumetricTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			preTemporalBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
				prevTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			cmd->ResourceBarrier(2, preTemporalBarriers);

			// Update temporal CB
			if (m_temporalCBMapped)
			{
				m_temporalCBMapped->InvViewProj = XMMatrixTranspose(invViewProj);
				m_temporalCBMapped->PrevViewProj = XMMatrixTranspose(m_prevViewProj);
				m_temporalCBMapped->CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 1.0f);
				m_temporalCBMapped->TemporalParams = XMFLOAT4(
					params.temporalBlend,
					static_cast<float>(m_quarterW),
					static_cast<float>(m_quarterH),
					static_cast<float>(frameIndex));
			}

			// Fill temporal descriptor heap: [0]=depth, [1]=current, [2]=prev, [3]=out(UAV)
			D3D12_CPU_DESCRIPTOR_HANDLE tCpuBase = m_temporalDescHeap->GetCPUDescriptorHandleForHeapStart();

			// [0] depth SRV (already in NON_PIXEL_SHADER_RESOURCE from vol pass)
			dev->CreateShaderResourceView(depthResource, &depthSrvDesc, tCpuBase);

			// [1] current volumetric SRV
			D3D12_CPU_DESCRIPTOR_HANDLE slot1 = tCpuBase; slot1.ptr += m_temporalDescStride;
			D3D12_SHADER_RESOURCE_VIEW_DESC volSrvDesc = {};
			volSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			volSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			volSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			volSrvDesc.Texture2D.MipLevels = 1;
			dev->CreateShaderResourceView(m_volumetricTex.Get(), &volSrvDesc, slot1);

			// [2] previous frame SRV
			D3D12_CPU_DESCRIPTOR_HANDLE slot2 = tCpuBase; slot2.ptr += 2 * m_temporalDescStride;
			dev->CreateShaderResourceView(prevTex, &volSrvDesc, slot2);

			// [3] output UAV
			D3D12_CPU_DESCRIPTOR_HANDLE slot3 = tCpuBase; slot3.ptr += 3 * m_temporalDescStride;
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			dev->CreateUnorderedAccessView(outTex, nullptr, &uavDesc, slot3);

			ID3D12DescriptorHeap* tHeaps[] = { m_temporalDescHeap.Get() };
			cmd->SetDescriptorHeaps(1, tHeaps);
			cmd->SetComputeRootSignature(m_temporalRootSig.Get());
			cmd->SetPipelineState(m_temporalPso.Get());

			cmd->SetComputeRootConstantBufferView(0, m_temporalCB->GetGPUVirtualAddress());
			D3D12_GPU_DESCRIPTOR_HANDLE tGpuBase = m_temporalDescHeap->GetGPUDescriptorHandleForHeapStart();
			cmd->SetComputeRootDescriptorTable(1, tGpuBase); // SRV table (t0,t1,t2)
			D3D12_GPU_DESCRIPTOR_HANDLE tUavGpu = tGpuBase;
			tUavGpu.ptr += 3 * m_temporalDescStride;
			cmd->SetComputeRootDescriptorTable(2, tUavGpu);  // UAV table (u0)

			cmd->Dispatch(groupsX, groupsY, 1);

			// Transition: outTex UAV→SRV for composite, volumetricTex SRV→UAV (reset)
			D3D12_RESOURCE_BARRIER postTemporalBarriers[3] = {};
			postTemporalBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
				outTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			postTemporalBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_volumetricTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			postTemporalBarriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(
				depthResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			cmd->ResourceBarrier(3, postTemporalBarriers);

			GPU_CMD_END_EVENT(cmd);
		}
		else
		{
			// No temporal: transition volumetric UAV→SRV, depth back to DSV
			D3D12_RESOURCE_BARRIER postBarriers[2] = {};
			postBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_volumetricTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
				depthResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			cmd->ResourceBarrier(2, postBarriers);
		}
	}
	else
	{
		// No volumetric: just transition volumetric and temporal output to SRV
		auto volToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			m_volumetricTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		cmd->ResourceBarrier(1, &volToSrv);
	}

	// ---- Fog + Volumetric composite (fullscreen PS) ----
	GPU_CMD_BEGIN_EVENT(cmd, 120, 160, 220, L"Atmosphere: Fog + Volumetric composite");

	// Determine which texture the composite reads:
	// If temporal ran, read the temporal output; otherwise read the raw volumetric
	ID3D12Resource* compositeVolSrc = nullptr;
	D3D12_RESOURCE_STATES compositeVolSrcCurrentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	if (doTemporal)
	{
		compositeVolSrc = outTex;  // already in PIXEL_SHADER_RESOURCE
	}
	else
	{
		compositeVolSrc = m_volumetricTex.Get(); // already in PIXEL_SHADER_RESOURCE
	}

	{
		auto* dev = g_Engine->Device();
		D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = m_compDescHeap->GetCPUDescriptorHandleForHeapStart();

		D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
		depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		depthSrvDesc.Texture2D.MipLevels = 1;
		dev->CreateShaderResourceView(depthResource, &depthSrvDesc, cpuBase);

		D3D12_CPU_DESCRIPTOR_HANDLE volSlot = cpuBase;
		volSlot.ptr += m_compDescStride;
		D3D12_SHADER_RESOURCE_VIEW_DESC volSrvDesc = {};
		volSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		volSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		volSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		volSrvDesc.Texture2D.MipLevels = 1;
		dev->CreateShaderResourceView(compositeVolSrc, &volSrvDesc, volSlot);
	}

	auto depthToPs = CD3DX12_RESOURCE_BARRIER::Transition(
		depthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &depthToPs);

	ID3D12DescriptorHeap* heaps[] = { m_compDescHeap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);

	D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(m_fullW), static_cast<float>(m_fullH), 0, 1 };
	D3D12_RECT sr = { 0, 0, static_cast<LONG>(m_fullW), static_cast<LONG>(m_fullH) };
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &sr);
	cmd->OMSetRenderTargets(1, &hdrSceneRtvCpu, FALSE, nullptr);

	cmd->SetGraphicsRootSignature(m_compRootSig.Get());
	cmd->SetPipelineState(m_compPso.Get());
	cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
	D3D12_GPU_DESCRIPTOR_HANDLE compGpuBase = m_compDescHeap->GetGPUDescriptorHandleForHeapStart();
	cmd->SetGraphicsRootDescriptorTable(1, compGpuBase);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->DrawInstanced(3, 1, 0, 0);

	// Transition depth back to DSV
	auto depthBackToDsv = CD3DX12_RESOURCE_BARRIER::Transition(
		depthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	cmd->ResourceBarrier(1, &depthBackToDsv);

	// Transition composite source back to UAV for next frame
	if (doTemporal)
	{
		// outTex: SRV→UAV (for next frame as prev input)
		auto outBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			outTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		// prevTex was already transitioned to NON_PIXEL→UAV? No, it's still NON_PIXEL_SHADER_RESOURCE.
		auto prevBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			prevTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		D3D12_RESOURCE_BARRIER barriers[] = { outBarrier, prevBarrier };
		cmd->ResourceBarrier(2, barriers);
		m_temporalPingPong = !m_temporalPingPong;
	}
	else
	{
		auto volBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_volumetricTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &volBarrier);
	}

	// Store current ViewProj for next frame's temporal reprojection
	m_prevViewProj = curViewProj;
	m_hasPrevViewProj = true;

	GPU_CMD_END_EVENT(cmd);
}
