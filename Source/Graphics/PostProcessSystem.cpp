#include "PostProcessSystem.h"
#include <d3dx12.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

bool PostProcessSystem::Init(ID3D12Device* device)
{
	if (device == nullptr) return false;

	CD3DX12_ROOT_PARAMETER rootParams[2] = {};
	rootParams[0].InitAsConstants(4, 0);
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	rootParams[1].InitAsDescriptorTable(1, &srvRange);

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 2;
	rsDesc.pParameters = rootParams;
	rsDesc.NumStaticSamplers = 1;
	rsDesc.pStaticSamplers = &sampler;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> vsBlob, psBlob;
	hr = D3DReadFileToBlob(L"ToneMap_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr)) hr = D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = D3DReadFileToBlob(L"ToneMap_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr)) hr = D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_pRootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.InputLayout = { nullptr, 0 };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pToneMapPSO));
	if (FAILED(hr)) return false;

	m_isValid = true;
	return true;
}

void PostProcessSystem::Execute(
	ID3D12GraphicsCommandList* cmdList,
	D3D12_GPU_DESCRIPTOR_HANDLE hdrSrvHandle,
	D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtvHandle,
	const PostProcessSettings& settings)
{
	if (!m_isValid || cmdList == nullptr) return;

	cmdList->OMSetRenderTargets(1, &backBufferRtvHandle, FALSE, nullptr);
	cmdList->SetGraphicsRootSignature(m_pRootSignature.Get());
	cmdList->SetPipelineState(m_pToneMapPSO.Get());
	cmdList->SetGraphicsRoot32BitConstants(0, 4, &settings, 0);
	cmdList->SetGraphicsRootDescriptorTable(1, hdrSrvHandle);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawInstanced(3, 1, 0, 0);
}
