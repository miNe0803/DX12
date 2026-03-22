#include "PostProcessSystem.h"

#include "DescriptorHeap.h"

#include "Engine.h"

#include <d3dx12.h>

#include <d3dcompiler.h>

#include <cstdio>

#include "GpuDebugLabels.h"

#pragma comment(lib, "d3dcompiler.lib")



static bool LoadShader(const wchar_t* name, ID3DBlob** outBlob)

{

	HRESULT hr = D3DReadFileToBlob(name, outBlob);

	if (SUCCEEDED(hr)) return true;

	// 実行 dir がプロジェクトルートの場合

	wchar_t alt[256];

	swprintf_s(alt, L"Shaders\\PostProcess\\%s", name);

	hr = D3DReadFileToBlob(alt, outBlob);

	return SUCCEEDED(hr);

}



bool PostProcessSystem::Init(ID3D12Device* device, DescriptorHeap* descriptorHeap, UINT width, UINT height, ID3D12Resource* nprHdrSceneColor)

{

	if (device == nullptr || descriptorHeap == nullptr || width == 0 || height == 0) return false;

	m_hasNprHdrSrv = false;

	m_nprHdrSrvGpu = {};



	m_width = width;

	m_height = height;

	m_bloomWidth = (width + 1) / 2;

	m_bloomHeight = (height + 1) / 2;



	// ---- フル解像度 LDR 中間 ×2（PBR トーン結果 / NPR トーン結果）----

	D3D12_CLEAR_VALUE ldrClear = {};

	ldrClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	ldrClear.Color[0] = ldrClear.Color[1] = ldrClear.Color[2] = 0.0f;

	ldrClear.Color[3] = 0.0f;

	auto heapPropDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	auto ldrResDesc = CD3DX12_RESOURCE_DESC::Tex2D(

		DXGI_FORMAT_R8G8B8A8_UNORM,

		width, height, 1, 1, 1, 0,

		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

	HRESULT hr = device->CreateCommittedResource(

		&heapPropDefault, D3D12_HEAP_FLAG_NONE, &ldrResDesc,

		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ldrClear,

		IID_PPV_ARGS(m_pPbrLdr.ReleaseAndGetAddressOf()));

	if (FAILED(hr)) return false;

	hr = device->CreateCommittedResource(

		&heapPropDefault, D3D12_HEAP_FLAG_NONE, &ldrResDesc,

		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ldrClear,

		IID_PPV_ARGS(m_pNprLdr.ReleaseAndGetAddressOf()));

	if (FAILED(hr)) return false;



	D3D12_DESCRIPTOR_HEAP_DESC ldrRtvDesc = {};

	ldrRtvDesc.NumDescriptors = 2;

	ldrRtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

	ldrRtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	hr = device->CreateDescriptorHeap(&ldrRtvDesc, IID_PPV_ARGS(&m_pLdrRtvHeap));

	if (FAILED(hr)) return false;

	m_ldrRtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	D3D12_RENDER_TARGET_VIEW_DESC ldrRtvView = {};

	ldrRtvView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	ldrRtvView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	m_pbrLdrRtvCpu = m_pLdrRtvHeap->GetCPUDescriptorHandleForHeapStart();

	device->CreateRenderTargetView(m_pPbrLdr.Get(), &ldrRtvView, m_pbrLdrRtvCpu);

	m_nprLdrRtvCpu = m_pbrLdrRtvCpu;

	m_nprLdrRtvCpu.ptr += m_ldrRtvDescriptorSize;

	device->CreateRenderTargetView(m_pNprLdr.Get(), &ldrRtvView, m_nprLdrRtvCpu);



	// ---- Bloom RT 3 枚 (1/2 解像度) ----

	D3D12_CLEAR_VALUE clearValue = {};

	clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = 0.0f;

	clearValue.Color[3] = 1.0f;

	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(

		DXGI_FORMAT_R16G16B16A16_FLOAT,

		m_bloomWidth, m_bloomHeight, 1, 1, 1, 0,

		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);



	ComPtr<ID3D12Resource>* bloomTargets[] = { &m_pBloomExtract, &m_pBloomBlurA, &m_pBloomBlurB };

	for (auto* target : bloomTargets)

	{

		hr = device->CreateCommittedResource(

			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,

			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,

			IID_PPV_ARGS(target->ReleaseAndGetAddressOf()));

		if (FAILED(hr)) return false;

	}



	// RTV ヒープ (3)

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};

	rtvDesc.NumDescriptors = 3;

	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

	rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_pBloomRtvHeap));

	if (FAILED(hr)) return false;

	m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);



	D3D12_RENDER_TARGET_VIEW_DESC rtvView = {};

	rtvView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	rtvView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pBloomRtvHeap->GetCPUDescriptorHandleForHeapStart();

	device->CreateRenderTargetView(m_pBloomExtract.Get(), &rtvView, rtvHandle);

	rtvHandle.ptr += m_rtvDescriptorSize;

	device->CreateRenderTargetView(m_pBloomBlurA.Get(), &rtvView, rtvHandle);

	rtvHandle.ptr += m_rtvDescriptorSize;

	device->CreateRenderTargetView(m_pBloomBlurB.Get(), &rtvView, rtvHandle);



	// SRV ヒープ (3) — Blur パスで中間 RT をバインドする用

	D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};

	srvDesc.NumDescriptors = 3;

	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	hr = device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_pBloomSrvHeap));

	if (FAILED(hr)) return false;

	m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);



	D3D12_SHADER_RESOURCE_VIEW_DESC srvView = {};

	srvView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	srvView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	srvView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

	srvView.Texture2D.MipLevels = 1;

	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_pBloomSrvHeap->GetCPUDescriptorHandleForHeapStart();

	D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_pBloomSrvHeap->GetGPUDescriptorHandleForHeapStart();

	device->CreateShaderResourceView(m_pBloomExtract.Get(), &srvView, srvCpu);

	m_bloomExtractSrvGpu = srvGpu;

	srvCpu.ptr += m_srvDescriptorSize;

	srvGpu.ptr += m_srvDescriptorSize;

	device->CreateShaderResourceView(m_pBloomBlurA.Get(), &srvView, srvCpu);

	m_bloomBlurASrvGpu = srvGpu;

	srvCpu.ptr += m_srvDescriptorSize;

	srvGpu.ptr += m_srvDescriptorSize;

	device->CreateShaderResourceView(m_pBloomBlurB.Get(), &srvView, srvCpu);

	m_bloomBlurBSrvGpu = srvGpu;



	// シーン用ヒープに Bloom 最終結果を登録（ToneMap で t1 として HDR と並べて使う）

	descriptorHeap->RegisterResource(m_pBloomBlurB.Get(), srvView);



	D3D12_SHADER_RESOURCE_VIEW_DESC ldrSrvView = {};

	ldrSrvView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	ldrSrvView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	ldrSrvView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

	ldrSrvView.Texture2D.MipLevels = 1;

	// Composite の t0,t1 は連続ディスクリプタ必須（順序: PBR LDR → NPR LDR）

	DescriptorHandle* hPbrLdrSrv = descriptorHeap->RegisterResource(m_pPbrLdr.Get(), ldrSrvView);

	if (!descriptorHeap->RegisterResource(m_pNprLdr.Get(), ldrSrvView) || !hPbrLdrSrv) return false;

	m_compositeLayersSrvGpuBase = hPbrLdrSrv->HandleGPU;

	if (nprHdrSceneColor)

	{

		D3D12_SHADER_RESOURCE_VIEW_DESC nprHdrSrvDesc = {};

		nprHdrSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

		nprHdrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		nprHdrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

		nprHdrSrvDesc.Texture2D.MipLevels = 1;

		DescriptorHandle* hNprHdr = descriptorHeap->RegisterResource(nprHdrSceneColor, nprHdrSrvDesc);

		if (!hNprHdr)

			fprintf(stderr, "[PostProcess] NPR HDR の SRV 登録に失敗（ヒープ上限など）。分割コンポは無効になります。\n");

		else

		{

			m_nprHdrSrvGpu = hNprHdr->HandleGPU;

			m_hasNprHdrSrv = true;

		}

	}



	// ---- Extract ルートシグネチャ・PSO ----

	CD3DX12_ROOT_PARAMETER extractRootParams[2] = {};

	extractRootParams[0].InitAsConstants(4, 0);

	CD3DX12_DESCRIPTOR_RANGE extractSrvRange;

	extractSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	extractRootParams[1].InitAsDescriptorTable(1, &extractSrvRange);

	D3D12_STATIC_SAMPLER_DESC pointSampler = {};

	pointSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;

	pointSampler.AddressU = pointSampler.AddressV = pointSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	pointSampler.ShaderRegister = 0;

	pointSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC extractRsDesc = {};

	extractRsDesc.NumParameters = 2;

	extractRsDesc.pParameters = extractRootParams;

	extractRsDesc.NumStaticSamplers = 1;

	extractRsDesc.pStaticSamplers = &pointSampler;

	extractRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;



	ComPtr<ID3DBlob> extractSig, extractErr;

	hr = D3D12SerializeRootSignature(&extractRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, extractSig.GetAddressOf(), extractErr.GetAddressOf());

	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, extractSig->GetBufferPointer(), extractSig->GetBufferSize(), IID_PPV_ARGS(&m_pExtractRootSignature));

	if (FAILED(hr)) return false;



	ComPtr<ID3DBlob> vsBlob, extractPsBlob, blurPsBlob, toneMapPsBlob, nprTonemapPsBlob, compositePsBlob;

	if (!LoadShader(L"ToneMap_VS.cso", vsBlob.GetAddressOf())) return false;

	if (!LoadShader(L"bloom_extract_ps.cso", extractPsBlob.GetAddressOf())) return false;

	if (!LoadShader(L"bloom_blur_ps.cso", blurPsBlob.GetAddressOf())) return false;

	if (!LoadShader(L"ToneMap_PS.cso", toneMapPsBlob.GetAddressOf())) return false;

	if (!LoadShader(L"NprTonemap_PS.cso", nprTonemapPsBlob.GetAddressOf())) return false;

	if (!LoadShader(L"Composite_PS.cso", compositePsBlob.GetAddressOf())) return false;



	D3D12_GRAPHICS_PIPELINE_STATE_DESC extractPsoDesc = {};

	extractPsoDesc.pRootSignature = m_pExtractRootSignature.Get();

	extractPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());

	extractPsoDesc.PS = CD3DX12_SHADER_BYTECODE(extractPsBlob.Get());

	extractPsoDesc.InputLayout = { nullptr, 0 };

	extractPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

	extractPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	extractPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	extractPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

	extractPsoDesc.DepthStencilState.DepthEnable = FALSE;

	extractPsoDesc.SampleMask = UINT_MAX;

	extractPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	extractPsoDesc.NumRenderTargets = 1;

	extractPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;

	extractPsoDesc.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&extractPsoDesc, IID_PPV_ARGS(&m_pExtractPSO));

	if (FAILED(hr)) return false;



	// ---- Blur ルートシグネチャ・PSO ----

	CD3DX12_ROOT_PARAMETER blurRootParams[2] = {};

	blurRootParams[0].InitAsConstants(8, 0);

	CD3DX12_DESCRIPTOR_RANGE blurSrvRange;

	blurSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	blurRootParams[1].InitAsDescriptorTable(1, &blurSrvRange);

	D3D12_STATIC_SAMPLER_DESC linearSampler = {};

	linearSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;

	linearSampler.AddressU = linearSampler.AddressV = linearSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	linearSampler.ShaderRegister = 0;

	linearSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC blurRsDesc = {};

	blurRsDesc.NumParameters = 2;

	blurRsDesc.pParameters = blurRootParams;

	blurRsDesc.NumStaticSamplers = 1;

	blurRsDesc.pStaticSamplers = &linearSampler;

	blurRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;



	ComPtr<ID3DBlob> blurSig, blurErr;

	hr = D3D12SerializeRootSignature(&blurRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, blurSig.GetAddressOf(), blurErr.GetAddressOf());

	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, blurSig->GetBufferPointer(), blurSig->GetBufferSize(), IID_PPV_ARGS(&m_pBlurRootSignature));

	if (FAILED(hr)) return false;



	D3D12_GRAPHICS_PIPELINE_STATE_DESC blurPsoDesc = {};

	blurPsoDesc.pRootSignature = m_pBlurRootSignature.Get();

	blurPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());

	blurPsoDesc.PS = CD3DX12_SHADER_BYTECODE(blurPsBlob.Get());

	blurPsoDesc.InputLayout = { nullptr, 0 };

	blurPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

	blurPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	blurPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	blurPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

	blurPsoDesc.DepthStencilState.DepthEnable = FALSE;

	blurPsoDesc.SampleMask = UINT_MAX;

	blurPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	blurPsoDesc.NumRenderTargets = 1;

	blurPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;

	blurPsoDesc.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&blurPsoDesc, IID_PPV_ARGS(&m_pBlurPSO));

	if (FAILED(hr)) return false;



	// ---- ToneMap ルートシグネチャ（2 SRV）・PSO ----

	CD3DX12_ROOT_PARAMETER toneMapRootParams[2] = {};

	toneMapRootParams[0].InitAsConstants(4, 0);

	CD3DX12_DESCRIPTOR_RANGE toneMapSrvRange;

	toneMapSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);  // t0=HDR, t1=Bloom

	toneMapRootParams[1].InitAsDescriptorTable(1, &toneMapSrvRange);

	D3D12_STATIC_SAMPLER_DESC toneMapSampler = {};

	toneMapSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;

	toneMapSampler.AddressU = toneMapSampler.AddressV = toneMapSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	toneMapSampler.ShaderRegister = 0;

	toneMapSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC toneMapRsDesc = {};

	toneMapRsDesc.NumParameters = 2;

	toneMapRsDesc.pParameters = toneMapRootParams;

	toneMapRsDesc.NumStaticSamplers = 1;

	toneMapRsDesc.pStaticSamplers = &toneMapSampler;

	toneMapRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;



	ComPtr<ID3DBlob> toneMapSig, toneMapErr;

	hr = D3D12SerializeRootSignature(&toneMapRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, toneMapSig.GetAddressOf(), toneMapErr.GetAddressOf());

	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, toneMapSig->GetBufferPointer(), toneMapSig->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));

	if (FAILED(hr)) return false;



	D3D12_GRAPHICS_PIPELINE_STATE_DESC toneMapPsoDesc = {};

	toneMapPsoDesc.pRootSignature = m_pRootSignature.Get();

	toneMapPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());

	toneMapPsoDesc.PS = CD3DX12_SHADER_BYTECODE(toneMapPsBlob.Get());

	toneMapPsoDesc.InputLayout = { nullptr, 0 };

	toneMapPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

	toneMapPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	toneMapPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	toneMapPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

	toneMapPsoDesc.DepthStencilState.DepthEnable = FALSE;

	toneMapPsoDesc.SampleMask = UINT_MAX;

	toneMapPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	toneMapPsoDesc.NumRenderTargets = 1;

	toneMapPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

	toneMapPsoDesc.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&toneMapPsoDesc, IID_PPV_ARGS(&m_pToneMapPSO));

	if (FAILED(hr)) return false;



	// ---- NPR Tonemap（1 SRV + 定数）----

	CD3DX12_ROOT_PARAMETER nprTmParams[2] = {};

	nprTmParams[0].InitAsConstants(4, 0);

	CD3DX12_DESCRIPTOR_RANGE nprTmSrv;

	nprTmSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	nprTmParams[1].InitAsDescriptorTable(1, &nprTmSrv);

	D3D12_ROOT_SIGNATURE_DESC nprTmRs = {};

	nprTmRs.NumParameters = 2;

	nprTmRs.pParameters = nprTmParams;

	nprTmRs.NumStaticSamplers = 1;

	nprTmRs.pStaticSamplers = &toneMapSampler;

	nprTmRs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> nprTmSig, nprTmErr;

	hr = D3D12SerializeRootSignature(&nprTmRs, D3D_ROOT_SIGNATURE_VERSION_1, nprTmSig.GetAddressOf(), nprTmErr.GetAddressOf());

	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, nprTmSig->GetBufferPointer(), nprTmSig->GetBufferSize(), IID_PPV_ARGS(&m_pNprTonemapRootSignature));

	if (FAILED(hr)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC nprTmPso = {};

	nprTmPso.pRootSignature = m_pNprTonemapRootSignature.Get();

	nprTmPso.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());

	nprTmPso.PS = CD3DX12_SHADER_BYTECODE(nprTonemapPsBlob.Get());

	nprTmPso.InputLayout = { nullptr, 0 };

	nprTmPso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

	nprTmPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	nprTmPso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	nprTmPso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

	nprTmPso.DepthStencilState.DepthEnable = FALSE;

	nprTmPso.SampleMask = UINT_MAX;

	nprTmPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	nprTmPso.NumRenderTargets = 1;

	nprTmPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

	nprTmPso.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&nprTmPso, IID_PPV_ARGS(&m_pNprTonemapPSO));

	if (FAILED(hr)) return false;



	// ---- Composite（2 SRV）----

	CD3DX12_ROOT_PARAMETER compParams[1] = {};

	CD3DX12_DESCRIPTOR_RANGE compSrv;

	compSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

	compParams[0].InitAsDescriptorTable(1, &compSrv);

	D3D12_ROOT_SIGNATURE_DESC compRs = {};

	compRs.NumParameters = 1;

	compRs.pParameters = compParams;

	compRs.NumStaticSamplers = 1;

	compRs.pStaticSamplers = &toneMapSampler;

	compRs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> compSig, compErr;

	hr = D3D12SerializeRootSignature(&compRs, D3D_ROOT_SIGNATURE_VERSION_1, compSig.GetAddressOf(), compErr.GetAddressOf());

	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, compSig->GetBufferPointer(), compSig->GetBufferSize(), IID_PPV_ARGS(&m_pCompositeRootSignature));

	if (FAILED(hr)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC compPso = {};

	compPso.pRootSignature = m_pCompositeRootSignature.Get();

	compPso.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());

	compPso.PS = CD3DX12_SHADER_BYTECODE(compositePsBlob.Get());

	compPso.InputLayout = { nullptr, 0 };

	compPso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

	compPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	compPso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	compPso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

	compPso.DepthStencilState.DepthEnable = FALSE;

	compPso.SampleMask = UINT_MAX;

	compPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	compPso.NumRenderTargets = 1;

	compPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

	compPso.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&compPso, IID_PPV_ARGS(&m_pCompositePSO));

	if (FAILED(hr)) return false;



	m_isValid = true;

	return true;

}



void PostProcessSystem::Execute(

	ID3D12GraphicsCommandList* cmdList,

	ID3D12DescriptorHeap* sceneDescriptorHeap,

	D3D12_GPU_DESCRIPTOR_HANDLE hdrSrvHandle,

	D3D12_CPU_DESCRIPTOR_HANDLE finalOutputRtvCpu,

	const PostProcessSettings& settings,

	bool compositeNprLayer)

{

	if (!m_isValid || cmdList == nullptr) return;

	const bool doNpr = compositeNprLayer && m_hasNprHdrSrv;



	// 定数: ToneMap 用 (exposure, gamma, bloomIntensity, padding)

	struct ToneMapConstants { float exposure, gamma, bloomIntensity, padding; };

	ToneMapConstants toneMapConst = { settings.exposure, settings.gamma, settings.bloomIntensity, 0.0f };



	struct NprTonemapConstants { float nprExposure, nprGamma, padding[2]; };

	NprTonemapConstants nprTmConst = { settings.nprPostExposure, settings.nprPostGamma, 0.0f, 0.0f };



	// Extract 用: threshold + kneeWidth（bloom_extract_ps）

	float extractConst[4] = { settings.threshold, settings.bloomKnee, 0.0f, 0.0f };



	// Blur 用 (texelSize.xy, direction.xy, blurSize, padding x3)

	float invW = 1.0f / static_cast<float>(m_bloomWidth);

	float invH = 1.0f / static_cast<float>(m_bloomHeight);

	float blurConstH[8] = { invW, invH, 1.0f, 0.0f, settings.blurSize, 0.0f, 0.0f, 0.0f };

	float blurConstV[8] = { invW, invH, 0.0f, 1.0f, settings.blurSize, 0.0f, 0.0f, 0.0f };



	GPU_CMD_BEGIN_EVENT(cmdList, 200, 230, 100, L"PostProcess: Bloom (extract + blur)");

	GPU_CMD_BEGIN_EVENT(cmdList, 205, 235, 115, L"  Bloom: extract (threshold pass)");

	// バリア: HDR は呼び出し元で SRV にしている前提。Bloom RT は最初 SRV。

	// Extract: bloomExtract を RTV に

	D3D12_RESOURCE_BARRIER toRtvExtract = CD3DX12_RESOURCE_BARRIER::Transition(

		m_pBloomExtract.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

	cmdList->ResourceBarrier(1, &toRtvExtract);



	// --- Extract パス（現在の descriptor heap = シーンのヒープで HDR を t0 にバインド）---

	D3D12_VIEWPORT halfViewport = { 0.0f, 0.0f, static_cast<float>(m_bloomWidth), static_cast<float>(m_bloomHeight), 0.0f, 1.0f };

	D3D12_RECT halfScissor = { 0, 0, static_cast<LONG>(m_bloomWidth), static_cast<LONG>(m_bloomHeight) };

	cmdList->RSSetViewports(1, &halfViewport);

	cmdList->RSSetScissorRects(1, &halfScissor);



	D3D12_CPU_DESCRIPTOR_HANDLE extractRtv = m_pBloomRtvHeap->GetCPUDescriptorHandleForHeapStart();

	cmdList->OMSetRenderTargets(1, &extractRtv, FALSE, nullptr);

	cmdList->SetGraphicsRootSignature(m_pExtractRootSignature.Get());

	cmdList->SetPipelineState(m_pExtractPSO.Get());

	cmdList->SetGraphicsRoot32BitConstants(0, 4, extractConst, 0);

	cmdList->SetGraphicsRootDescriptorTable(1, hdrSrvHandle);

	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	cmdList->DrawInstanced(3, 1, 0, 0);

	GPU_CMD_END_EVENT(cmdList);

	GPU_CMD_BEGIN_EVENT(cmdList, 195, 220, 125, L"  Bloom: blur horizontal");

	// bloomExtract → SRV

	D3D12_RESOURCE_BARRIER toSrvExtract = CD3DX12_RESOURCE_BARRIER::Transition(

		m_pBloomExtract.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// BlurA を RTV に

	D3D12_RESOURCE_BARRIER toRtvBlurA = CD3DX12_RESOURCE_BARRIER::Transition(

		m_pBloomBlurA.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

	D3D12_RESOURCE_BARRIER barriers1[2] = { toSrvExtract, toRtvBlurA };

	cmdList->ResourceBarrier(2, barriers1);



	// --- Blur 横（Bloom SRV ヒープ使用）---

	D3D12_CPU_DESCRIPTOR_HANDLE blurARtv = m_pBloomRtvHeap->GetCPUDescriptorHandleForHeapStart();

	blurARtv.ptr += m_rtvDescriptorSize;

	ID3D12DescriptorHeap* bloomHeaps[] = { m_pBloomSrvHeap.Get() };

	cmdList->SetDescriptorHeaps(1, bloomHeaps);

	cmdList->OMSetRenderTargets(1, &blurARtv, FALSE, nullptr);

	cmdList->SetGraphicsRootSignature(m_pBlurRootSignature.Get());

	cmdList->SetPipelineState(m_pBlurPSO.Get());

	cmdList->SetGraphicsRoot32BitConstants(0, 8, blurConstH, 0);

	cmdList->SetGraphicsRootDescriptorTable(1, m_bloomExtractSrvGpu);

	cmdList->DrawInstanced(3, 1, 0, 0);

	GPU_CMD_END_EVENT(cmdList);

	GPU_CMD_BEGIN_EVENT(cmdList, 195, 210, 135, L"  Bloom: blur vertical + finalize SRV");

	// BlurA → SRV, BlurB → RTV

	D3D12_RESOURCE_BARRIER toSrvBlurA = CD3DX12_RESOURCE_BARRIER::Transition(

		m_pBloomBlurA.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D12_RESOURCE_BARRIER toRtvBlurB = CD3DX12_RESOURCE_BARRIER::Transition(

		m_pBloomBlurB.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

	D3D12_RESOURCE_BARRIER barriers2[2] = { toSrvBlurA, toRtvBlurB };

	cmdList->ResourceBarrier(2, barriers2);



	// --- Blur 縦 ---

	D3D12_CPU_DESCRIPTOR_HANDLE blurBRtv = m_pBloomRtvHeap->GetCPUDescriptorHandleForHeapStart();

	blurBRtv.ptr += m_rtvDescriptorSize * 2;

	cmdList->OMSetRenderTargets(1, &blurBRtv, FALSE, nullptr);

	cmdList->SetGraphicsRoot32BitConstants(0, 8, blurConstV, 0);

	cmdList->SetGraphicsRootDescriptorTable(1, m_bloomBlurASrvGpu);

	cmdList->DrawInstanced(3, 1, 0, 0);



	// BlurB → SRV（ToneMap で t1 として参照；シーンヒープに登録済みなのでヒープ切替後に hdrSrvHandle が 2 連続で HDR, Bloom）

	D3D12_RESOURCE_BARRIER toSrvBlurB = CD3DX12_RESOURCE_BARRIER::Transition(

		m_pBloomBlurB.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	cmdList->ResourceBarrier(1, &toSrvBlurB);

	GPU_CMD_END_EVENT(cmdList);

	GPU_CMD_END_EVENT(cmdList);



	// --- ToneMap（シーンの descriptor heap）---

	if (sceneDescriptorHeap)

		cmdList->SetDescriptorHeaps(1, &sceneDescriptorHeap);

	float fullViewportW = static_cast<float>(m_width);

	float fullViewportH = static_cast<float>(m_height);

	D3D12_VIEWPORT fullViewport = { 0.0f, 0.0f, fullViewportW, fullViewportH, 0.0f, 1.0f };

	D3D12_RECT fullScissor = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

	cmdList->RSSetViewports(1, &fullViewport);

	cmdList->RSSetScissorRects(1, &fullScissor);



	if (doNpr)

	{

		GPU_CMD_BEGIN_EVENT(cmdList, 90, 145, 220, L"PostProcess: Split path (PBR LDR + NPR LDR + composite)");

		GPU_CMD_BEGIN_EVENT(cmdList, 120, 180, 255, L"  [1] PBR LDR: barrier (SRV→RTV)");

		D3D12_RESOURCE_BARRIER toPbrLdrRtv = CD3DX12_RESOURCE_BARRIER::Transition(

			m_pPbrLdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

		cmdList->ResourceBarrier(1, &toPbrLdrRtv);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_BEGIN_EVENT(cmdList, 115, 175, 250, L"  [2] PBR LDR: fullscreen (Bloom+ACES tonemap)");

		cmdList->OMSetRenderTargets(1, &m_pbrLdrRtvCpu, FALSE, nullptr);

		cmdList->SetGraphicsRootSignature(m_pRootSignature.Get());

		cmdList->SetPipelineState(m_pToneMapPSO.Get());

		cmdList->SetGraphicsRoot32BitConstants(0, 4, &toneMapConst, 0);

		cmdList->SetGraphicsRootDescriptorTable(1, hdrSrvHandle);

		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		cmdList->DrawInstanced(3, 1, 0, 0);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_BEGIN_EVENT(cmdList, 160, 160, 180, L"  [3] Barriers: PBR LDR→SRV, NPR LDR→RTV");

		D3D12_RESOURCE_BARRIER pbrLdrToSrv = CD3DX12_RESOURCE_BARRIER::Transition(

			m_pPbrLdr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		D3D12_RESOURCE_BARRIER nprLdrToRtv = CD3DX12_RESOURCE_BARRIER::Transition(

			m_pNprLdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

		D3D12_RESOURCE_BARRIER barriersTm[2] = { pbrLdrToSrv, nprLdrToRtv };

		cmdList->ResourceBarrier(2, barriersTm);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_BEGIN_EVENT(cmdList, 255, 140, 200, L"  [4] NPR LDR: fullscreen (no ACES tonemap)");

		cmdList->OMSetRenderTargets(1, &m_nprLdrRtvCpu, FALSE, nullptr);

		cmdList->SetGraphicsRootSignature(m_pNprTonemapRootSignature.Get());

		cmdList->SetPipelineState(m_pNprTonemapPSO.Get());

		cmdList->SetGraphicsRoot32BitConstants(0, 4, &nprTmConst, 0);

		cmdList->SetGraphicsRootDescriptorTable(1, m_nprHdrSrvGpu);

		cmdList->DrawInstanced(3, 1, 0, 0);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_BEGIN_EVENT(cmdList, 200, 200, 200, L"  [5] Barrier: NPR LDR→SRV");

		D3D12_RESOURCE_BARRIER nprLdrToSrv = CD3DX12_RESOURCE_BARRIER::Transition(

			m_pNprLdr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		cmdList->ResourceBarrier(1, &nprLdrToSrv);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_BEGIN_EVENT(cmdList, 180, 255, 160, L"  [6] Composite: premul PBR+NPR (fullscreen)");

		cmdList->OMSetRenderTargets(1, &finalOutputRtvCpu, FALSE, nullptr);

		cmdList->SetGraphicsRootSignature(m_pCompositeRootSignature.Get());

		cmdList->SetPipelineState(m_pCompositePSO.Get());

		cmdList->SetGraphicsRootDescriptorTable(0, m_compositeLayersSrvGpuBase);

		cmdList->DrawInstanced(3, 1, 0, 0);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_END_EVENT(cmdList);

	}

	else

	{

		GPU_CMD_BEGIN_EVENT(cmdList, 200, 200, 255, L"PostProcess: Tonemap to backbuffer (single HDR)");

		GPU_CMD_BEGIN_EVENT(cmdList, 190, 195, 245, L"  Single path: bind + fullscreen tonemap draw");

		cmdList->OMSetRenderTargets(1, &finalOutputRtvCpu, FALSE, nullptr);

		cmdList->SetGraphicsRootSignature(m_pRootSignature.Get());

		cmdList->SetPipelineState(m_pToneMapPSO.Get());

		cmdList->SetGraphicsRoot32BitConstants(0, 4, &toneMapConst, 0);

		cmdList->SetGraphicsRootDescriptorTable(1, hdrSrvHandle);

		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		cmdList->DrawInstanced(3, 1, 0, 0);

		GPU_CMD_END_EVENT(cmdList);

		GPU_CMD_END_EVENT(cmdList);

	}

}

