#include "SkyboxRenderer.h"
#include "Engine.h"
#include "DebugLog.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

namespace
{
	// HLSL constant buffer is 256-byte aligned.
	const UINT SkyboxCBSize = 256;

	struct SkyboxCB
	{
		DirectX::XMFLOAT4X4 InvProjT;        // transpose(invProj)
		DirectX::XMFLOAT4X4 InvViewNoTransT; // transpose(invViewNoTrans)
	};
}

bool SkyboxRenderer::Init(ID3D12Device* device, ID3D12Resource* cubemap, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle)
{
	DebugLog("[SkyboxRenderer] Init begin\n");
	if (device == nullptr || cubemap == nullptr)
	{
		DebugLog("[SkyboxRenderer] Init failed: device or cubemap null\n");
		return false;
	}

	m_cubemapSRV = srvHandle;
	m_pCubemap = cubemap;

	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_ROOT_PARAMETER rootParams[2] = {};
	rootParams[0].InitAsConstantBufferView(0);
	rootParams[1].InitAsDescriptorTable(1, &srvRange);

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	// 【重要な修正】WRAP だと面の端で反対側のピクセルを拾って「線」が出ることがあるため、CLAMP に変更します
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 2;
	rsDesc.pParameters = rootParams;
	rsDesc.NumStaticSamplers = 1;
	rsDesc.pStaticSamplers = &sampler;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> rsBlob, rsError;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsError.GetAddressOf());
	if (FAILED(hr)) return false;

	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_pRootSignature.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> vsBlob, psBlob;
	hr = D3DReadFileToBlob(L"Skybox_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr)) hr = D3DReadFileToBlob(L"Shaders\\Skybox\\Skybox_VS.cso", vsBlob.GetAddressOf());
	if (FAILED(hr)) return false;

	hr = D3DReadFileToBlob(L"Skybox_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr)) hr = D3DReadFileToBlob(L"Shaders\\Skybox\\Skybox_PS.cso", psBlob.GetAddressOf());
	if (FAILED(hr)) return false;

	// フルスクリーン三角形は SV_VertexID のみ使用。頂点バッファ不要のため InputLayout は空
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_pRootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.InputLayout = { nullptr, 0 };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.DepthBias = 0;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;  // 深度は書かない
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;  // Z=1.0 に描画
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR メイン描画先
	psoDesc.DSVFormat = Engine::kDepthStencilFormat;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pPSO.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	{
		auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(SkyboxCBSize);
		hr = device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pConstantBuffer));
		if (FAILED(hr)) return false;
	}

	m_isValid = true;
	DebugLog("[SkyboxRenderer] Init end OK\n");
	return true;
}

void SkyboxRenderer::Draw(
	ID3D12GraphicsCommandList* commandList,
	const XMMATRIX& view,
	const XMMATRIX& proj)
{
	if (!m_isValid || commandList == nullptr) return;

	// ① View の平行移動を消す（常にカメラ中心＝無限遠の空）
	XMMATRIX viewNoTrans = view;
	viewNoTrans.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

	// Fullscreen sky: reconstruct ray from inverse matrices
	XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
	// viewNoTrans is orthonormal rotation, so inverse = transpose
	XMMATRIX invViewNoTrans = XMMatrixTranspose(viewNoTrans);

	void* mapped = nullptr;
	if (SUCCEEDED(m_pConstantBuffer->Map(0, nullptr, &mapped)))
	{
		SkyboxCB cb{};
		XMStoreFloat4x4(&cb.InvProjT, XMMatrixTranspose(invProj));
		XMStoreFloat4x4(&cb.InvViewNoTransT, XMMatrixTranspose(invViewNoTrans));
		memcpy(mapped, &cb, sizeof(cb));
		m_pConstantBuffer->Unmap(0, nullptr);
	}

	commandList->SetGraphicsRootSignature(m_pRootSignature.Get());
	commandList->SetPipelineState(m_pPSO.Get());
	commandList->SetGraphicsRootConstantBufferView(0, m_pConstantBuffer->GetGPUVirtualAddress());
	commandList->SetGraphicsRootDescriptorTable(1, m_cubemapSRV);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->DrawInstanced(3, 1, 0, 0);
}
