#include "IBLGenerator.h"
#include "DebugLog.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <vector>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

#include "EXRLoader.h"

static std::string WideToUtf8(const wchar_t* wpath)
{
	if (wpath == nullptr) return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 0) return {};
	std::string out(static_cast<size_t>(len), 0);
	WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &out[0], len, nullptr, nullptr);
	out.resize(out.find('\0'));
	return out;
}

struct Equirect2CubeParams
{
	UINT faceIndex;
	UINT width;
	UINT height;
	UINT padding;
};

bool IBLGenerator::Generate(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	const wchar_t* exrPath,
	UINT cubemapSize,
	ID3D12Resource** outCubemap,
	std::function<void()> executeAndWait,
	ID3D12Resource** outEquirectTexture)
{
	if (device == nullptr || commandList == nullptr || exrPath == nullptr || outCubemap == nullptr || cubemapSize == 0 || !executeAndWait)
	{
		DebugLog("[IBL] Generate: invalid args -> false\n");
		return false;
	}
	*outCubemap = nullptr;

	DebugLog("[IBL] EXR loader: OpenEXR\n");

	int eqW = 0, eqH = 0;
	float* rgba = nullptr;
	if (!LoadEXRToScratch(exrPath, &eqW, &eqH, &rgba))
	{
		DebugLog("[IBL] LoadEXRToScratch failed\n");
		return false;
	}
	DebugLog("[IBL] LoadEXRToScratch OK (%dx%d)\n", eqW, eqH);

	m_pEquirectTexture.Reset();
	if (!CreateEquirectTexture(device, commandList, eqW, eqH, rgba, m_pEquirectTexture.GetAddressOf()))
	{
		free(rgba);
		DebugLog("[IBL] CreateEquirectTexture failed\n");
		return false;
	}
	free(rgba);
	rgba = nullptr;
	DebugLog("[IBL] CreateEquirectTexture OK\n");

	ComPtr<ID3D12Resource> cubemapResource;
	if (!CreateCubemapResource(device, cubemapSize, cubemapResource.GetAddressOf()))
	{
		DebugLog("[IBL] CreateCubemapResource failed\n");
		return false;
	}
	DebugLog("[IBL] CreateCubemapResource OK (%u)\n", cubemapSize);

	if (!CreateComputePipeline(device))
	{
		DebugLog("[IBL] CreateComputePipeline failed (Equirect2Cube_CS.cso?)\n");
		return false;
	}
	DebugLog("[IBL] CreateComputePipeline OK\n");

	DebugLog("[IBL] RunEquirect2Cube(%u)...\n", cubemapSize);
	RunEquirect2Cube(device, commandList, m_pEquirectTexture.Get(), cubemapResource.Get(), cubemapSize);
	DebugLog("[IBL] RunEquirect2Cube done\n");

	// コマンドリストをここで実行する（全参照リソースがまだスコープ内のため #921 を防ぐ）
	DebugLog("[IBL] ExecuteAndWait (inside Generate)...\n");
	executeAndWait();
	DebugLog("[IBL] ExecuteAndWait done\n");

	*outCubemap = cubemapResource.Detach();
	if (outEquirectTexture != nullptr && m_pEquirectTexture.Get() != nullptr)
	{
		m_pEquirectTexture->AddRef();
		*outEquirectTexture = m_pEquirectTexture.Get();
	}
	m_isValid = true;
	DebugLog("[IBL] Generate OK -> cubemap ready\n");
	return true;
}

bool IBLGenerator::GenerateIrradianceMap(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* envCubemap,
	std::function<void()> executeAndWait,
	ID3D12Resource** outIrradianceCubemap)
{
	if (device == nullptr || commandList == nullptr || envCubemap == nullptr || outIrradianceCubemap == nullptr || !executeAndWait)
	{
		DebugLog("[IBL] GenerateIrradianceMap: invalid args -> false\n");
		return false;
	}
	*outIrradianceCubemap = nullptr;

	const UINT irradianceSize = 32u;
	ComPtr<ID3D12Resource> irradianceCubemap;
	if (!CreateIrradianceCubemapResource(device, irradianceSize, irradianceCubemap.GetAddressOf()))
	{
		DebugLog("[IBL] GenerateIrradianceMap: CreateIrradianceCubemapResource failed\n");
		return false;
	}
	DebugLog("[IBL] CreateIrradianceCubemapResource OK (%u)\n", irradianceSize);

	if (!CreateIrradiancePipeline(device))
	{
		DebugLog("[IBL] GenerateIrradianceMap: CreateIrradiancePipeline failed (IrradianceMap_CS.cso?)\n");
		return false;
	}
	DebugLog("[IBL] RunIrradianceMap(%u)...\n", irradianceSize);
	RunIrradianceMap(device, commandList, envCubemap, irradianceCubemap.Get(), irradianceSize);
	DebugLog("[IBL] RunIrradianceMap done\n");

	DebugLog("[IBL] ExecuteAndWait (irradiance)...\n");
	executeAndWait();
	DebugLog("[IBL] ExecuteAndWait done\n");

	*outIrradianceCubemap = irradianceCubemap.Detach();
	DebugLog("[IBL] GenerateIrradianceMap OK -> irradiance cubemap ready\n");
	return true;
}

bool IBLGenerator::CreateIrradianceCubemapResource(ID3D12Device* device, UINT size, ID3D12Resource** outResource)
{
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		size, size,
		6, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(outResource));
	return SUCCEEDED(hr);
}

bool IBLGenerator::CreateIrradiancePipeline(ID3D12Device* device)
{
	if (m_pIrradiancePSO.Get() != nullptr)
		return true;
	// フォールバック時は Equirect2Cube を呼ばないためルートシグネチャが未作成。その場合はここで作成する（レイアウトは Equirect と同じ b0, t0, u0）
	if (m_pComputeRootSignature.Get() == nullptr)
	{
		CD3DX12_DESCRIPTOR_RANGE srvRange;
		srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE uavRange;
		uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER rootParams[3] = {};
		rootParams[0].InitAsConstantBufferView(0);
		rootParams[1].InitAsDescriptorTable(1, &srvRange);
		rootParams[2].InitAsDescriptorTable(1, &uavRange);
		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 3;
		rsDesc.pParameters = rootParams;
		rsDesc.NumStaticSamplers = 1;
		rsDesc.pStaticSamplers = &sampler;
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		ComPtr<ID3DBlob> rsBlob, rsError;
		HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsError.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_pComputeRootSignature.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
	}
	ComPtr<ID3DBlob> csBlob;
	HRESULT hr = D3DReadFileToBlob(L"Shaders\\IBL\\IrradianceMap_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"IrradianceMap_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr)) return false;
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_pComputeRootSignature.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_pIrradiancePSO.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

void IBLGenerator::RunIrradianceMap(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* envCubemap,
	ID3D12Resource* irradianceCubemap,
	UINT size)
{
	const UINT numDescriptors = 7;
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = numDescriptors;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	m_pIrradianceDescriptorHeap.Reset();
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pIrradianceDescriptorHeap));
	if (FAILED(hr)) return;

	ID3D12DescriptorHeap* heap = m_pIrradianceDescriptorHeap.Get();
	UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MipLevels = 1;
	device->CreateShaderResourceView(envCubemap, &srvDesc, cpuBase);

	for (UINT face = 0; face < 6; face++)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpuBase;
		uavHandle.ptr += increment * (1u + face);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.ArraySize = 1;
		uavDesc.Texture2DArray.FirstArraySlice = face;
		uavDesc.Texture2DArray.MipSlice = 0;
		device->CreateUnorderedAccessView(irradianceCubemap, nullptr, &uavDesc, uavHandle);
	}

	m_pIrradianceParamsBuffer.Reset();
	{
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
		hr = device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pIrradianceParamsBuffer));
		if (FAILED(hr)) return;
	}
	ID3D12Resource* paramsBuffer = m_pIrradianceParamsBuffer.Get();

	commandList->SetComputeRootSignature(m_pComputeRootSignature.Get());
	commandList->SetPipelineState(m_pIrradiancePSO.Get());
	commandList->SetDescriptorHeaps(1, &heap);

	D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = heap->GetGPUDescriptorHandleForHeapStart();

	struct IrradianceParams { UINT faceIndex; UINT width; UINT padding0; UINT padding1; };
	for (UINT face = 0; face < 6; face++)
	{
		IrradianceParams params = {};
		params.faceIndex = face;
		params.width = size;
		void* mapped = nullptr;
		paramsBuffer->Map(0, nullptr, &mapped);
		memcpy(mapped, &params, sizeof(params));
		paramsBuffer->Unmap(0, nullptr);

		commandList->SetComputeRootConstantBufferView(0, paramsBuffer->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(1, gpuBase);
		D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = gpuBase;
		uavHandle.ptr += increment * (1u + face);
		commandList->SetComputeRootDescriptorTable(2, uavHandle);

		UINT groupsX = (size + 7) / 8;
		UINT groupsY = (size + 7) / 8;
		commandList->Dispatch(groupsX, groupsY, 1);
	}

	D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(irradianceCubemap);
	commandList->ResourceBarrier(1, &uavBarrier);
	D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(irradianceCubemap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &toSrv);
}

bool IBLGenerator::GeneratePrefilteredEnvMap(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* envCubemap,
	std::function<void()> executeAndWait,
	ID3D12Resource** outPrefilterCubemap)
{
	if (device == nullptr || commandList == nullptr || envCubemap == nullptr || outPrefilterCubemap == nullptr || !executeAndWait)
	{
		DebugLog("[IBL] GeneratePrefilteredEnvMap: invalid args -> false\n");
		return false;
	}
	*outPrefilterCubemap = nullptr;

	const UINT prefilterSize = 128u;
	const UINT mipLevels = 5u;
	ComPtr<ID3D12Resource> prefilterCubemap;
	if (!CreatePrefilterCubemapResource(device, prefilterSize, mipLevels, prefilterCubemap.GetAddressOf()))
	{
		DebugLog("[IBL] GeneratePrefilteredEnvMap: CreatePrefilterCubemapResource failed\n");
		return false;
	}
	DebugLog("[IBL] CreatePrefilterCubemapResource OK (%u, mips=%u)\n", prefilterSize, mipLevels);

	if (!CreatePrefilterPipeline(device))
	{
		DebugLog("[IBL] GeneratePrefilteredEnvMap: CreatePrefilterPipeline failed (Prefilter_CS.cso?)\n");
		return false;
	}
	DebugLog("[IBL] RunPrefilterEnv...\n");
	RunPrefilterEnv(device, commandList, envCubemap, prefilterCubemap.Get(), prefilterSize, mipLevels);
	DebugLog("[IBL] RunPrefilterEnv done\n");

	executeAndWait();

	*outPrefilterCubemap = prefilterCubemap.Detach();
	DebugLog("[IBL] GeneratePrefilteredEnvMap OK -> prefilter cubemap ready\n");
	return true;
}

bool IBLGenerator::CreatePrefilterCubemapResource(ID3D12Device* device, UINT size, UINT mipLevels, ID3D12Resource** outResource)
{
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		size, size,
		6, mipLevels, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(outResource));
	return SUCCEEDED(hr);
}

bool IBLGenerator::CreatePrefilterPipeline(ID3D12Device* device)
{
	if (m_pPrefilterPSO.Get() != nullptr)
		return true;
	if (m_pComputeRootSignature.Get() == nullptr)
	{
		CD3DX12_DESCRIPTOR_RANGE srvRange;
		srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE uavRange;
		uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER rootParams[3] = {};
		rootParams[0].InitAsConstantBufferView(0);
		rootParams[1].InitAsDescriptorTable(1, &srvRange);
		rootParams[2].InitAsDescriptorTable(1, &uavRange);
		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 3;
		rsDesc.pParameters = rootParams;
		rsDesc.NumStaticSamplers = 1;
		rsDesc.pStaticSamplers = &sampler;
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		ComPtr<ID3DBlob> rsBlob, rsError;
		HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsError.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_pComputeRootSignature.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
	}
	ComPtr<ID3DBlob> csBlob;
	HRESULT hr = D3DReadFileToBlob(L"Prefilter_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"Shaders\\IBL\\Prefilter_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr)) return false;
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_pComputeRootSignature.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_pPrefilterPSO.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

void IBLGenerator::RunPrefilterEnv(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* envCubemap,
	ID3D12Resource* prefilterCubemap,
	UINT size,
	UINT mipLevels)
{
	const UINT numUavs = 6 * mipLevels;
	const UINT numDescriptors = 1 + numUavs;
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = numDescriptors;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	m_pPrefilterDescriptorHeap.Reset();
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pPrefilterDescriptorHeap));
	if (FAILED(hr)) return;

	ID3D12DescriptorHeap* heap = m_pPrefilterDescriptorHeap.Get();
	UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MipLevels = 1;
	device->CreateShaderResourceView(envCubemap, &srvDesc, cpuBase);

	for (UINT mip = 0; mip < mipLevels; mip++)
		for (UINT face = 0; face < 6; face++)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpuBase;
			uavHandle.ptr += increment * (1u + face * mipLevels + mip);
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.ArraySize = 1;
			uavDesc.Texture2DArray.FirstArraySlice = face;
			uavDesc.Texture2DArray.MipSlice = mip;
			device->CreateUnorderedAccessView(prefilterCubemap, nullptr, &uavDesc, uavHandle);
		}

	m_pPrefilterParamsBuffer.Reset();
	{
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
		hr = device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pPrefilterParamsBuffer));
		if (FAILED(hr)) return;
	}
	struct PrefilterParams { UINT faceIndex; UINT width; UINT mipLevel; float roughness; };
	ID3D12Resource* paramsBuffer = m_pPrefilterParamsBuffer.Get();

	commandList->SetComputeRootSignature(m_pComputeRootSignature.Get());
	commandList->SetPipelineState(m_pPrefilterPSO.Get());
	commandList->SetDescriptorHeaps(1, &heap);

	D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = heap->GetGPUDescriptorHandleForHeapStart();

	for (UINT mip = 0; mip < mipLevels; mip++)
	{
		UINT mipWidth = (UINT)(size >> mip);
		if (mipWidth < 1) mipWidth = 1;
		float roughness = (float)mip / (float)(mipLevels - 1);
		for (UINT face = 0; face < 6; face++)
		{
			PrefilterParams params = {};
			params.faceIndex = face;
			params.width = mipWidth;
			params.mipLevel = mip;
			params.roughness = roughness;
			void* mapped = nullptr;
			paramsBuffer->Map(0, nullptr, &mapped);
			memcpy(mapped, &params, sizeof(params));
			paramsBuffer->Unmap(0, nullptr);

			commandList->SetComputeRootConstantBufferView(0, paramsBuffer->GetGPUVirtualAddress());
			commandList->SetComputeRootDescriptorTable(1, gpuBase);
			D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = gpuBase;
			uavHandle.ptr += increment * (1u + face * mipLevels + mip);
			commandList->SetComputeRootDescriptorTable(2, uavHandle);

			UINT groupsX = (mipWidth + 7) / 8;
			UINT groupsY = (mipWidth + 7) / 8;
			commandList->Dispatch(groupsX, groupsY, 1);
		}
	}

	D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(prefilterCubemap);
	commandList->ResourceBarrier(1, &uavBarrier);
	D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(prefilterCubemap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &toSrv);
}

bool IBLGenerator::GenerateBrdfLut(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	std::function<void()> executeAndWait,
	ID3D12Resource** outBrdfLutTexture)
{
	if (device == nullptr || commandList == nullptr || outBrdfLutTexture == nullptr || !executeAndWait)
	{
		DebugLog("[IBL] GenerateBrdfLut: invalid args -> false\n");
		return false;
	}
	*outBrdfLutTexture = nullptr;

	const UINT lutSize = 512u;
	ComPtr<ID3D12Resource> brdfLutResource;
	if (!CreateBrdfLutResource(device, lutSize, lutSize, brdfLutResource.GetAddressOf()))
	{
		DebugLog("[IBL] GenerateBrdfLut: CreateBrdfLutResource failed\n");
		return false;
	}
	DebugLog("[IBL] CreateBrdfLutResource OK (%ux%u)\n", lutSize, lutSize);

	if (!CreateBrdfLutPipeline(device))
	{
		DebugLog("[IBL] GenerateBrdfLut: CreateBrdfLutPipeline failed (BRDF_Lut_CS.cso?)\n");
		return false;
	}
	DebugLog("[IBL] RunBrdfLut...\n");
	RunBrdfLut(device, commandList, brdfLutResource.Get(), lutSize, lutSize);
	DebugLog("[IBL] RunBrdfLut done\n");

	executeAndWait();

	*outBrdfLutTexture = brdfLutResource.Detach();
	DebugLog("[IBL] GenerateBrdfLut OK -> BRDF LUT ready\n");
	return true;
}

bool IBLGenerator::CreateBrdfLutResource(ID3D12Device* device, UINT width, UINT height, ID3D12Resource** outResource)
{
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16_FLOAT,
		width, height,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(outResource));
	return SUCCEEDED(hr);
}

bool IBLGenerator::CreateBrdfLutPipeline(ID3D12Device* device)
{
	if (m_pBrdfLutPSO.Get() != nullptr)
		return true;

	CD3DX12_DESCRIPTOR_RANGE uavRange;
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	CD3DX12_ROOT_PARAMETER rootParams[2] = {};
	rootParams[0].InitAsConstantBufferView(0);
	rootParams[1].InitAsDescriptorTable(1, &uavRange);

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 2;
	rsDesc.pParameters = rootParams;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsBlob, rsError;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsError.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_pBrdfLutRootSignature.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	hr = D3DReadFileToBlob(L"BRDF_Lut_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"Shaders\\IBL\\BRDF_Lut_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr)) return false;

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_pBrdfLutRootSignature.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_pBrdfLutPSO.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

void IBLGenerator::RunBrdfLut(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* brdfLutResource,
	UINT width,
	UINT height)
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	m_pBrdfLutDescriptorHeap.Reset();
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pBrdfLutDescriptorHeap));
	if (FAILED(hr)) return;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	device->CreateUnorderedAccessView(brdfLutResource, nullptr, &uavDesc, m_pBrdfLutDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	m_pBrdfLutParamsBuffer.Reset();
	{
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
		hr = device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pBrdfLutParamsBuffer));
		if (FAILED(hr)) return;
	}
	struct BrdfLutParams { UINT width; UINT height; UINT padding0; UINT padding1; };
	BrdfLutParams params = {};
	params.width = width;
	params.height = height;
	void* mapped = nullptr;
	m_pBrdfLutParamsBuffer->Map(0, nullptr, &mapped);
	memcpy(mapped, &params, sizeof(params));
	m_pBrdfLutParamsBuffer->Unmap(0, nullptr);

	ID3D12DescriptorHeap* heap = m_pBrdfLutDescriptorHeap.Get();
	commandList->SetComputeRootSignature(m_pBrdfLutRootSignature.Get());
	commandList->SetPipelineState(m_pBrdfLutPSO.Get());
	commandList->SetDescriptorHeaps(1, &heap);
	commandList->SetComputeRootConstantBufferView(0, m_pBrdfLutParamsBuffer->GetGPUVirtualAddress());
	commandList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());

	UINT groupsX = (width + 7) / 8;
	UINT groupsY = (height + 7) / 8;
	commandList->Dispatch(groupsX, groupsY, 1);

	D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(brdfLutResource);
	commandList->ResourceBarrier(1, &uavBarrier);
	D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(brdfLutResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &toSrv);
}

// EXR 読み込み（EXRLoader = OpenEXR を別 TU で使用）。false の場合、Generate() は失敗し Scene 側でフォールバックになる。
bool IBLGenerator::LoadEXRToScratch(const wchar_t* exrPath, int* outW, int* outH, float** outRgba)
{
	if (exrPath == nullptr || outW == nullptr || outH == nullptr || outRgba == nullptr)
		return false;
	*outW = 0;
	*outH = 0;
	*outRgba = nullptr;

	std::string path = WideToUtf8(exrPath);
	if (path.empty())
	{
		DebugLog("[IBL] OpenEXR: path conversion failed\n");
		return false;
	}

	if (!LoadEXRToFloatRgba(path.c_str(), outW, outH, outRgba))
	{
		DebugLog("[IBL] LoadEXRToFloatRgba failed (path=%s)\n", path.c_str());
		return false;
	}
	DebugLog("[IBL] OpenEXR LoadEXR OK: %dx%d, path=%s\n", *outW, *outH, path.c_str());
	return true;
}

bool IBLGenerator::CreateEquirectTexture(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, int width, int height, const float* rgba, ID3D12Resource** outResource)
{
	if (rgba == nullptr || width <= 0 || height <= 0 || outResource == nullptr || commandList == nullptr)
		return false;

	const UINT rowPitch = static_cast<UINT>(width) * 4 * sizeof(float);
	const UINT totalBytes = rowPitch * static_cast<UINT>(height);

	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);

	m_pEquirectUploadBuffer.Reset();
	HRESULT hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_pEquirectUploadBuffer));
	if (FAILED(hr)) return false;

	void* mapped = nullptr;
	hr = m_pEquirectUploadBuffer->Map(0, nullptr, &mapped);
	if (FAILED(hr)) return false;
	memcpy(mapped, rgba, totalBytes);
	m_pEquirectUploadBuffer->Unmap(0, nullptr);

	auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		static_cast<UINT64>(width),
		static_cast<UINT>(height),
		1, 1);

	auto texHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	ComPtr<ID3D12Resource> tex;
	hr = device->CreateCommittedResource(
		&texHeap,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&tex));
	if (FAILED(hr)) return false;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	footprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	footprint.Footprint.Width = static_cast<UINT>(width);
	footprint.Footprint.Height = static_cast<UINT>(height);
	footprint.Footprint.Depth = 1;
	footprint.Footprint.RowPitch = rowPitch;
	footprint.Offset = 0;

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = tex.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = m_pEquirectUploadBuffer.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = footprint;

	commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &barrier);

	*outResource = tex.Detach();
	return true;
}

bool IBLGenerator::CreateCubemapResource(ID3D12Device* device, UINT size, ID3D12Resource** outResource)
{
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		size, size,
		6, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(outResource));
	return SUCCEEDED(hr);
}

bool IBLGenerator::CreateComputePipeline(ID3D12Device* device)
{
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	CD3DX12_DESCRIPTOR_RANGE uavRange;
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

	CD3DX12_ROOT_PARAMETER rootParams[3] = {};
	rootParams[0].InitAsConstantBufferView(0);
	rootParams[1].InitAsDescriptorTable(1, &srvRange);
	rootParams[2].InitAsDescriptorTable(1, &uavRange);

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = rootParams;
	rsDesc.NumStaticSamplers = 1;
	rsDesc.pStaticSamplers = &sampler;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsBlob, rsError;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsError.GetAddressOf());
	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_pComputeRootSignature.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	hr = D3DReadFileToBlob(L"Shaders\\IBL\\Equirect2Cube_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"Equirect2Cube_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr)) return false;

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_pComputeRootSignature.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());

	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_pComputePSO.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

void IBLGenerator::RunEquirect2Cube(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* equirectTexture,
	ID3D12Resource* cubemapResource,
	UINT size)
{
	const UINT numDescriptors = 7; // 1 SRV + 6 UAVs
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = numDescriptors;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	m_pComputeDescriptorHeap.Reset();
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pComputeDescriptorHeap));
	if (FAILED(hr)) return;

	ID3D12DescriptorHeap* heap = m_pComputeDescriptorHeap.Get();
	UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(equirectTexture, &srvDesc, cpuBase);

	for (UINT face = 0; face < 6; face++)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpuBase;
		uavHandle.ptr += increment * (1u + face);

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.ArraySize = 1;
		uavDesc.Texture2DArray.FirstArraySlice = face;
		uavDesc.Texture2DArray.MipSlice = 0;
		device->CreateUnorderedAccessView(cubemapResource, nullptr, &uavDesc, uavHandle);
	}

	// Params constant buffer (256-byte aligned) — コマンドリストが参照するためメンバで保持
	m_pParamsBuffer.Reset();
	{
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
		hr = device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pParamsBuffer));
		if (FAILED(hr)) return;
	}
	ID3D12Resource* paramsBuffer = m_pParamsBuffer.Get();

	commandList->SetComputeRootSignature(m_pComputeRootSignature.Get());
	commandList->SetPipelineState(m_pComputePSO.Get());
	commandList->SetDescriptorHeaps(1, &heap);

	D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = heap->GetGPUDescriptorHandleForHeapStart();

	for (UINT face = 0; face < 6; face++)
	{
		Equirect2CubeParams params = {};
		params.faceIndex = face;
		params.width = size;
		params.height = size;

		void* mapped = nullptr;
		paramsBuffer->Map(0, nullptr, &mapped);
		memcpy(mapped, &params, sizeof(params));
		paramsBuffer->Unmap(0, nullptr);

		commandList->SetComputeRootConstantBufferView(0, paramsBuffer->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(1, gpuBase);
		D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = gpuBase;
		uavHandle.ptr += increment * (1u + face);
		commandList->SetComputeRootDescriptorTable(2, uavHandle);

		UINT groupsX = (size + 7) / 8;
		UINT groupsY = (size + 7) / 8;
		commandList->Dispatch(groupsX, groupsY, 1);
	}

	D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(cubemapResource);
	commandList->ResourceBarrier(1, &uavBarrier);

	D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(cubemapResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &toSrv);
}

// RowPitch は D3D12 で 256 アラインが必要
static const UINT kTextureRowPitchAlignment = 256;

bool IBLGenerator::CreateDefaultCubemap(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	UINT size,
	float r, float g, float b, float a,
	ID3D12Resource** outCubemap,
	ID3D12Resource** outUploadBufferToKeep)
{
	if (device == nullptr || commandList == nullptr || outCubemap == nullptr || size == 0)
	{
		DebugLog("[IBL] CreateDefaultCubemap: invalid args\n");
		return false;
	}
	*outCubemap = nullptr;
	if (outUploadBufferToKeep) *outUploadBufferToKeep = nullptr;

	DebugLog("[IBL] CreateDefaultCubemap(size=%u) -> checkered per-face fallback (rgba args ignored)\n", size);
	// 1x1 キューブマップ用：1面あたり RowPitch 256 で 6 面
	const UINT rowPitch = (size * 4 * sizeof(float) + kTextureRowPitchAlignment - 1) & ~(kTextureRowPitchAlignment - 1);
	const UINT64 uploadSize = rowPitch * size * 6;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = size;
	texDesc.Height = size;
	texDesc.DepthOrArraySize = 6;
	texDesc.MipLevels = 1;
	texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	ComPtr<ID3D12Resource> cubemap;
	HRESULT hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&cubemap));
	if (FAILED(hr))
	{
		DebugLog("[IBL] CreateDefaultCubemap: CreateCommittedResource(cubemap) failed hr=0x%08X\n", (unsigned)hr);
		return false;
	}

	heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto uploadBufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
	ComPtr<ID3D12Resource> uploadBuffer;
	hr = device->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&uploadBufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadBuffer));
	if (FAILED(hr))
	{
		DebugLog("[IBL] CreateDefaultCubemap: CreateCommittedResource(upload) failed hr=0x%08X\n", (unsigned)hr);
		return false;
	}

	void* mapped = nullptr;
	hr = uploadBuffer->Map(0, nullptr, &mapped);
	if (FAILED(hr))
	{
		DebugLog("[IBL] CreateDefaultCubemap: Map failed hr=0x%08X\n", (unsigned)hr);
		return false;
	}

	// 暫定: 面ごとに色を変え＋チェック柄にして「フォールバックが描画されているか」を判別しやすくする
	// 面 0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z → 赤/緑/青/黄/シアン/マゼンタ
	static const float faceColors[6][4] = {
		{ 1.f, 0.f, 0.f, 1.f },  // +X 赤
		{ 0.f, 1.f, 0.f, 1.f },  // -X 緑
		{ 0.f, 0.f, 1.f, 1.f },  // +Y 青
		{ 1.f, 1.f, 0.f, 1.f },  // -Y 黄
		{ 0.f, 1.f, 1.f, 1.f },  // +Z シアン
		{ 1.f, 0.f, 1.f, 1.f },  // -Z マゼンタ
	};
	const UINT checkerTile = 4u; // 4x4 でチェック
	for (UINT face = 0; face < 6; face++)
	{
		const float* fc = faceColors[face];
		char* faceBase = static_cast<char*>(mapped) + face * (rowPitch * size);
		for (UINT y = 0; y < size; y++)
		{
			float* row = reinterpret_cast<float*>(faceBase + y * rowPitch);
			for (UINT x = 0; x < size; x++)
			{
				// チェック: (x/tile + y/tile) が偶数なら明るく、奇数なら暗く
				UINT tx = x / checkerTile, ty = y / checkerTile;
				float t = ((tx + ty) % 2u) ? 0.4f : 1.0f;
				row[x * 4 + 0] = fc[0] * t;
				row[x * 4 + 1] = fc[1] * t;
				row[x * 4 + 2] = fc[2] * t;
				row[x * 4 + 3] = fc[3];
			}
		}
	}
	uploadBuffer->Unmap(0, nullptr);

	// 手動で PlacedFootprint を設定してコピー
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	footprint.Offset = 0;
	footprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	footprint.Footprint.Width = size;
	footprint.Footprint.Height = size;
	footprint.Footprint.Depth = 1;
	footprint.Footprint.RowPitch = rowPitch;

	for (UINT face = 0; face < 6; face++)
	{
		footprint.Offset = face * (rowPitch * size);

		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource = cubemap.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = face;

		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.pResource = uploadBuffer.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = footprint;

		commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(cubemap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &toSrv);

	*outCubemap = cubemap.Detach();
	if (outUploadBufferToKeep)
		*outUploadBufferToKeep = uploadBuffer.Detach(); // 呼び出し側で Execute 完了まで保持すること
	DebugLog("[IBL] CreateDefaultCubemap OK\n");
	return true;
}
