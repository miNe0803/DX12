#include "RootSignature.h"
#include "Engine.h"
#include <d3dx12.h>

RootSignature::RootSignature(bool forTerrain)
{
	auto flag = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_ROOT_PARAMETER rootParam[5] = {};
	CD3DX12_DESCRIPTOR_RANGE tableRange[2] = {};

	if (forTerrain)
	{
		// Terrain (legacy): CBV0 transform, CBV1 terrain constants, table masks, table IBL
		rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
		rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
		tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
		tableRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 4);
		rootParam[2].InitAsDescriptorTable(1, &tableRange[0], D3D12_SHADER_VISIBILITY_PIXEL);
		rootParam[3].InitAsDescriptorTable(1, &tableRange[1], D3D12_SHADER_VISIBILITY_PIXEL);

		auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = 4;
		desc.NumStaticSamplers = 1;
		desc.pParameters = rootParam;
		desc.pStaticSamplers = &sampler;
		desc.Flags = flag;

		ComPtr<ID3DBlob> pBlob;
		ComPtr<ID3DBlob> pErrorBlob;
		auto hr = D3D12SerializeRootSignature(
			&desc,
			D3D_ROOT_SIGNATURE_VERSION_1_0,
			pBlob.GetAddressOf(),
			pErrorBlob.GetAddressOf());
		if (FAILED(hr))
		{
			printf("Terrain root signature serialize failed\n");
			return;
		}

		hr = g_Engine->Device()->CreateRootSignature(
			0,
			pBlob->GetBufferPointer(),
			pBlob->GetBufferSize(),
			IID_PPV_ARGS(m_pRootSignature.GetAddressOf()));
		if (FAILED(hr))
		{
			printf("Terrain root signature create failed\n");
			return;
		}

		m_IsValid = true;
		return;
	}

	// PBR instanced: CBV0 scene (b0,s0), CBV1 material (b1,s0), Root SRV instances (t0,s1),
	// table materials (t0-t3,s0), table IBL (t4-t6,s0)
	rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
	rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[2].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);
	tableRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 4);
	rootParam[3].InitAsDescriptorTable(1, &tableRange[0], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[4].InitAsDescriptorTable(1, &tableRange[1], D3D12_SHADER_VISIBILITY_PIXEL);

	auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 5;
	desc.NumStaticSamplers = 1;
	desc.pParameters = rootParam;
	desc.pStaticSamplers = &sampler;
	desc.Flags = flag;

	ComPtr<ID3DBlob> pBlob;
	ComPtr<ID3DBlob> pErrorBlob;
	auto hr = D3D12SerializeRootSignature(
		&desc,
		D3D_ROOT_SIGNATURE_VERSION_1_0,
		pBlob.GetAddressOf(),
		pErrorBlob.GetAddressOf());
	if (FAILED(hr))
	{
		printf("PBR root signature serialize failed\n");
		return;
	}

	hr = g_Engine->Device()->CreateRootSignature(
		0,
		pBlob->GetBufferPointer(),
		pBlob->GetBufferSize(),
		IID_PPV_ARGS(m_pRootSignature.GetAddressOf()));
	if (FAILED(hr))
	{
		printf("PBR root signature create failed\n");
		return;
	}

	m_IsValid = true;
}

bool RootSignature::IsValid()
{
	return m_IsValid;
}

ID3D12RootSignature* RootSignature::Get()
{
	return m_pRootSignature.Get();
}
