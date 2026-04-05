#include "RootSignature.h"
#include "Engine.h"
#include <d3dx12.h>

RootSignature::RootSignature(bool forTerrain)
{
	auto flag = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_ROOT_PARAMETER rootParam[10] = {};
	CD3DX12_DESCRIPTOR_RANGE tableRange[4] = {};

	if (forTerrain)
	{
		// Terrain: CBV0 transform, CBV1 terrain constants, table masks+groundTex(t0-t3), table IBL(t4-t6), root SRV payload(t0,s1)
		rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
		rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
		tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);
		tableRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 4);
		rootParam[2].InitAsDescriptorTable(1, &tableRange[0], D3D12_SHADER_VISIBILITY_PIXEL);
		rootParam[3].InitAsDescriptorTable(1, &tableRange[1], D3D12_SHADER_VISIBILITY_PIXEL);
		rootParam[4].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX);

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
	// table materials (t0-t5: albedo,normal,metallic,roughness,ramp,sphere), table IBL (t6-t8,s0)
	rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
	rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[2].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0);
	tableRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 6);
	rootParam[3].InitAsDescriptorTable(1, &tableRange[0], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[4].InitAsDescriptorTable(1, &tableRange[1], D3D12_SHADER_VISIBILITY_PIXEL);
	// Trees (GPU-driven): additional inputs for TreeIndirectVS only.
	// - Root SRV t1 space1: StructuredBuffer<uint> visibleIndex
	// - Root constants b2 space1 (8x32-bit): VisibleBase + インポスター用 footLocal(xyz)+halfW+height（float を uint ビットで）
	rootParam[5].InitAsShaderResourceView(1, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	rootParam[6].InitAsConstants(8, 2, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	// Shadow mapping (space2): shadow map SRV + shadow constant buffer
	tableRange[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2); // t0, space2
	rootParam[7].InitAsDescriptorTable(1, &tableRange[2], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[8].InitAsConstantBufferView(1, 2, D3D12_SHADER_VISIBILITY_PIXEL); // b1, space2

	D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
	samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
	// PCF comparison sampler for shadow mapping
	samplers[1] = {};
	samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	samplers[1].ShaderRegister = 1;
	samplers[1].RegisterSpace = 2;
	samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 9;
	desc.NumStaticSamplers = 2;
	desc.pParameters = rootParam;
	desc.pStaticSamplers = samplers;
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
