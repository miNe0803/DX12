#include "RootSignature.h"
#include "Engine.h"
#include <d3dx12.h>
#include <stdio.h>

// ----- Legacy constructor (backward compat) -----
RootSignature::RootSignature(bool forTerrain)
{
	CreateLegacy(forTerrain);
}

// ----- Typed constructor -----
RootSignature::RootSignature(RootSigType type)
{
	switch (type)
	{
	case RootSigType::PbrLegacy:      CreateLegacy(false); break;
	case RootSigType::TerrainLegacy:  CreateLegacy(true);  break;
	case RootSigType::Bindless:       CreateBindless(false); break;
	case RootSigType::TerrainBindless: CreateBindless(true); break;
	}
}

// =====================================================================
// Bindless root signature (SM6.6 ResourceDescriptorHeap)
// =====================================================================
void RootSignature::CreateBindless(bool forTerrain)
{
	CD3DX12_ROOT_PARAMETER1 rootParam[4] = {};

	// Param 0: Root CBV b0 s0 — SceneConstants
	rootParam[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, D3D12_SHADER_VISIBILITY_ALL);
	// Param 1: Root CBV b1 s0 — ShadowConstants (PBR) or TerrainConstants (Terrain)
	rootParam[1].InitAsConstantBufferView(1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, D3D12_SHADER_VISIBILITY_ALL);
	// Param 2: Root 32-bit constants b2 s0 — 4 DWORDs (materialBufSrvIdx, instanceBufSrvIdx, drawID, reserved)
	rootParam[2].InitAsConstants(4, 2, 0, D3D12_SHADER_VISIBILITY_ALL);
	// Param 3: Root SRV t0 s1 — Meshlet/tree extra data (optional, used by AS/MS/VS)
	rootParam[3].InitAsShaderResourceView(0, 1, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);

	// Static samplers
	D3D12_STATIC_SAMPLER_DESC samplers[3] = {};

	// s0: Linear wrap (general textures)
	samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

	// s1 space2: PCF comparison sampler (shadow mapping)
	samplers[1] = {};
	samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	samplers[1].ShaderRegister = 1;
	samplers[1].RegisterSpace = 0;
	samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// s2: Point clamp (Hi-Z, depth reads)
	samplers[2] = CD3DX12_STATIC_SAMPLER_DESC(
		2, D3D12_FILTER_MIN_MAG_MIP_POINT,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | // kept during IA→MS transition
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
	desc.Init_1_1(_countof(rootParam), rootParam, _countof(samplers), samplers, flags);

	ComPtr<ID3DBlob> pBlob;
	ComPtr<ID3DBlob> pErrorBlob;
	auto hr = D3DX12SerializeVersionedRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_1,
		pBlob.GetAddressOf(), pErrorBlob.GetAddressOf());
	if (FAILED(hr))
	{
		if (pErrorBlob) printf("Bindless root sig serialize: %s\n", (char*)pErrorBlob->GetBufferPointer());
		printf("Bindless root signature serialize failed (terrain=%d)\n", forTerrain);
		return;
	}

	hr = g_Engine->Device()->CreateRootSignature(
		0, pBlob->GetBufferPointer(), pBlob->GetBufferSize(),
		IID_PPV_ARGS(m_pRootSignature.GetAddressOf()));
	if (FAILED(hr))
	{
		printf("Bindless root signature create failed (terrain=%d)\n", forTerrain);
		return;
	}

	m_IsValid = true;
	printf("RootSignature: Bindless (terrain=%d) created OK.\n", forTerrain);
}

// =====================================================================
// Legacy root signature (existing code, preserved for transition)
// =====================================================================
void RootSignature::CreateLegacy(bool forTerrain)
{
	auto flag = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_ROOT_PARAMETER rootParam[10] = {};
	CD3DX12_DESCRIPTOR_RANGE tableRange[4] = {};

	if (forTerrain)
	{
		rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
		// b1 (TerrainParams) は Water_VS が CameraPos.w=time を読むため VS でも見える必要あり
		rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
		tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0); // t0-t5: tree_mask, nature_mask, ground_diff, ground_disp, rivers, snow
		tableRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 6); // t6-t8: IBL
		// t0-t5 は VS でも使う (TerrainVS が rivers/snow を参照して地形を変位するため)
		rootParam[2].InitAsDescriptorTable(1, &tableRange[0], D3D12_SHADER_VISIBILITY_ALL);
		rootParam[3].InitAsDescriptorTable(1, &tableRange[1], D3D12_SHADER_VISIBILITY_PIXEL);
		rootParam[4].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
		tableRange[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2); // t0, space2
		rootParam[5].InitAsDescriptorTable(1, &tableRange[2], D3D12_SHADER_VISIBILITY_PIXEL);
		rootParam[6].InitAsConstantBufferView(1, 2, D3D12_SHADER_VISIBILITY_PIXEL); // b1, space2
		// 拡張テレインテクスチャ t9-t12: Rivers_Direction, WaterColor_Color, Trees2_FreshWater, INHIBITORS_Out
		tableRange[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 9);
		rootParam[7].InitAsDescriptorTable(1, &tableRange[3], D3D12_SHADER_VISIBILITY_PIXEL);

		D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
		samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
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
		desc.NumParameters = 8; // +1 for extra terrain texture table at t9-t12
		desc.NumStaticSamplers = 2;
		desc.pParameters = rootParam;
		desc.pStaticSamplers = samplers;
		desc.Flags = flag;

		ComPtr<ID3DBlob> pBlob;
		ComPtr<ID3DBlob> pErrorBlob;
		auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
			pBlob.GetAddressOf(), pErrorBlob.GetAddressOf());
		if (FAILED(hr)) { printf("Terrain root signature serialize failed\n"); return; }

		hr = g_Engine->Device()->CreateRootSignature(0, pBlob->GetBufferPointer(), pBlob->GetBufferSize(),
			IID_PPV_ARGS(m_pRootSignature.GetAddressOf()));
		if (FAILED(hr)) { printf("Terrain root signature create failed\n"); return; }

		m_IsValid = true;
		return;
	}

	// PBR instanced (legacy)
	rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
	rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[2].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0);
	tableRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 6);
	rootParam[3].InitAsDescriptorTable(1, &tableRange[0], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[4].InitAsDescriptorTable(1, &tableRange[1], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[5].InitAsShaderResourceView(1, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	rootParam[6].InitAsConstants(8, 2, 1, D3D12_SHADER_VISIBILITY_VERTEX);
	tableRange[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2);
	rootParam[7].InitAsDescriptorTable(1, &tableRange[2], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParam[8].InitAsConstantBufferView(1, 2, D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
	samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
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
	auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
		pBlob.GetAddressOf(), pErrorBlob.GetAddressOf());
	if (FAILED(hr)) { printf("PBR root signature serialize failed\n"); return; }

	hr = g_Engine->Device()->CreateRootSignature(0, pBlob->GetBufferPointer(), pBlob->GetBufferSize(),
		IID_PPV_ARGS(m_pRootSignature.GetAddressOf()));
	if (FAILED(hr)) { printf("PBR root signature create failed\n"); return; }

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
