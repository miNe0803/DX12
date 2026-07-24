#include "GroundProbeSystem.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

struct GPCB { XMFLOAT3 origin; float tMax; };   // 16B

bool GroundProbeSystem::Init(ID3D12Device* device, uint32_t frameCount)
{
	if (!device) return false;
	m_frameCount = (frameCount < 1) ? 1 : (frameCount > 3 ? 3 : frameCount);

	// CB (upload, 16B)
	{
		auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto rd = CD3DX12_RESOURCE_DESC::Buffer(256);   // CB は 256B アライン
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb)))) return false;
		if (FAILED(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped)))) return false;
		memset(m_cbMapped, 0, 256);
	}
	// 出力 UAV バッファ（DEFAULT, 16B）
	{
		auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto rd = CD3DX12_RESOURCE_DESC::Buffer(16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_out)))) return false;
		GPU_SET_NAME(m_out.Get(), L"GroundProbe:out");
		m_outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	// readback バッファ（フレーム毎）
	for (uint32_t i = 0; i < m_frameCount; ++i)
	{
		auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
		auto rd = CD3DX12_RESOURCE_DESC::Buffer(16);
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback[i])))) return false;
	}

	// rootsig: b0 CBV, t0 TLAS(root SRV), u0 出力(root UAV)
	{
		CD3DX12_ROOT_PARAMETER params[3] = {};
		params[0].InitAsConstantBufferView(0);       // b0
		params[1].InitAsShaderResourceView(0);       // t0 = TLAS
		params[2].InitAsUnorderedAccessView(0);      // u0 = 出力（root UAV）
		D3D12_ROOT_SIGNATURE_DESC rs = {};
		rs.NumParameters = 3; rs.pParameters = params;
		ComPtr<ID3DBlob> sig, err;
		if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
		{ if (err) printf("GroundProbe RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
		if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
			IID_PPV_ARGS(&m_rootSig)))) return false;
	}
	// PSO（GroundProbe_CS.cso）
	{
		ComPtr<ID3DBlob> cs;
		if (FAILED(D3DReadFileToBlob(L"GroundProbe_CS.cso", &cs)) &&
			FAILED(D3DReadFileToBlob(L"Shaders\\RT\\GroundProbe_CS.cso", &cs)))
		{ printf("GroundProbe: GroundProbe_CS.cso not found\n"); return false; }
		D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = m_rootSig.Get();
		pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
		if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso)))) return false;
	}

	m_valid = true;
	printf("GroundProbeSystem::Init: OK\n");
	return true;
}

void GroundProbeSystem::Execute(ID3D12GraphicsCommandList4* cmd, D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA,
	const XMFLOAT3& origin, float tMax, uint32_t frameIndex)
{
	if (!m_valid || !cmd || !tlasGpuVA || frameIndex >= m_frameCount) return;

	auto* cb = reinterpret_cast<GPCB*>(m_cbMapped);
	cb->origin = origin;
	cb->tMax = tMax;

	if (m_outState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_out.Get(), m_outState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b); m_outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	cmd->SetComputeRootSignature(m_rootSig.Get());
	cmd->SetPipelineState(m_pso.Get());
	cmd->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
	cmd->SetComputeRootShaderResourceView(1, tlasGpuVA);
	cmd->SetComputeRootUnorderedAccessView(2, m_out->GetGPUVirtualAddress());
	cmd->Dispatch(1, 1, 1);

	// 出力を readback へコピー（UAV→COPY_SOURCE→戻す）
	auto toSrc = CD3DX12_RESOURCE_BARRIER::Transition(m_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->ResourceBarrier(1, &toSrc);
	cmd->CopyBufferRegion(m_readback[frameIndex].Get(), 0, m_out.Get(), 0, 16);
	auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(m_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->ResourceBarrier(1, &toUav);
	m_outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

bool GroundProbeSystem::Read(uint32_t frameIndex, float& outGroundY) const
{
	if (!m_valid || frameIndex >= m_frameCount || !m_readback[frameIndex]) return false;
	float* p = nullptr;
	D3D12_RANGE rr{ 0, 16 };
	if (FAILED(m_readback[frameIndex]->Map(0, &rr, reinterpret_cast<void**>(&p))) || !p) return false;
	float y = p[0]; float hit = p[3];
	D3D12_RANGE wr{ 0, 0 };
	m_readback[frameIndex]->Unmap(0, &wr);
	if (hit < 0.5f) return false;
	outGroundY = y;
	return true;
}

void GroundProbeSystem::Shutdown()
{
	if (m_cb && m_cbMapped) { m_cb->Unmap(0, nullptr); m_cbMapped = nullptr; }
	m_valid = false;
}
