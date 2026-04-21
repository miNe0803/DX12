#include "WaterSystem.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include "../../SharedStruct.h"
#include "../../PipelineState.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool WaterSystem::Init(ID3D12Device* device, DescriptorHeap* heap,
	ID3D12RootSignature* rootSig, uint32_t screenW, uint32_t screenH)
{
	if (!device || !heap) return false;
	m_screenW = screenW;
	m_screenH = screenH;

	if (!CreateConstantBuffer(device)) return false;
	if (!CreatePipeline(device, rootSig)) return false;

	m_valid = true;
	printf("WaterSystem::Init: OK\n");
	return true;
}

void WaterSystem::Shutdown()
{
	if (m_waterCB && m_waterCBMapped) { m_waterCB->Unmap(0, nullptr); m_waterCBMapped = nullptr; }
	m_waterPso.Reset(); m_waterCB.Reset();
	m_valid = false;
}

bool WaterSystem::CreateConstantBuffer(ID3D12Device* device)
{
	auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	auto hr = device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &cbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_waterCB));
	if (FAILED(hr)) return false;
	D3D12_RANGE r{0,0};
	m_waterCB->Map(0, &r, &m_waterCBMapped);
	return true;
}

bool WaterSystem::CreatePipeline(ID3D12Device* device, ID3D12RootSignature* rootSig)
{
	if (!rootSig)
	{
		printf("WaterSystem: no root signature, PSO deferred\n");
		return true;
	}

	// Load water VS/PS
	ComPtr<ID3DBlob> vsBlob, psBlob;
	HRESULT hr = D3DReadFileToBlob(L"Water_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr)) hr = D3DReadFileToBlob(L"Shaders\\Water_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr))
	{
		printf("WaterSystem: Water_VS.cso not found (compile later)\n");
		return true; // non-fatal
	}

	hr = D3DReadFileToBlob(L"Water_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr)) hr = D3DReadFileToBlob(L"Shaders\\Water_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr))
	{
		printf("WaterSystem: Water_PS.cso not found (compile later)\n");
		return true;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = rootSig;
	desc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	desc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	desc.InputLayout = Vertex::InputLayout;
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // water visible from both sides
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc = { 1, 0 };

	hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_waterPso.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		printf("WaterSystem: PSO create failed 0x%08X\n", hr);
		return false;
	}
	return true;
}

void WaterSystem::Draw(ID3D12GraphicsCommandList* cmd,
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
	D3D12_GPU_VIRTUAL_ADDRESS sceneCBAddress,
	ID3D12DescriptorHeap* srvHeap,
	float time)
{
	if (!m_valid || !m_waterPso || !cmd) return;

	// Update water constants
	if (m_waterCBMapped)
	{
		auto* cb = static_cast<WaterCBData*>(m_waterCBMapped);
		cb->WaterParams = XMFLOAT4(time, settings.surfaceY, settings.foamWidth, settings.specPower);
		cb->AbsorptionCoeff = XMFLOAT4(settings.absorptionRGB.x, settings.absorptionRGB.y,
			settings.absorptionRGB.z, settings.absorptionScale);
		cb->WaterColor = XMFLOAT4(settings.shallowColor.x, settings.shallowColor.y,
			settings.shallowColor.z, 1.0f);
		cb->NormalScroll1 = XMFLOAT4(settings.scroll1Dir.x, settings.scroll1Dir.y,
			settings.scroll1Speed, settings.scroll1Scale);
		cb->NormalScroll2 = XMFLOAT4(settings.scroll2Dir.x, settings.scroll2Dir.y,
			settings.scroll2Speed, settings.scroll2Scale);
	}

	// Set pipeline state
	cmd->SetPipelineState(m_waterPso.Get());
	cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	// Bind scene CB (b0) and water CB (b1)
	cmd->SetGraphicsRootConstantBufferView(0, sceneCBAddress);
	cmd->SetGraphicsRootConstantBufferView(1, m_waterCB->GetGPUVirtualAddress());

	// Water geometry: reuse terrain VB/IB for water-masked cells
	// The actual draw call uses the terrain mesh with water mask filtering.
	// In the simplest form: draw a fullscreen quad at water height, depth-tested.
	// Integration with terrain VB/IB will be done in Scene.cpp.

	// Placeholder: caller provides the terrain VB/IB and does the actual DrawIndexed.
}
