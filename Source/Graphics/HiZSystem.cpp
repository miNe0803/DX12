#include "HiZSystem.h"
#include "DebugLog.h"
#include <d3dcompiler.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace {

bool LoadHiZCsBlob(const wchar_t* pathPrimary, const wchar_t* pathAlt, ComPtr<ID3DBlob>& outBlob)
{
	HRESULT hr = D3DReadFileToBlob(pathPrimary, outBlob.ReleaseAndGetAddressOf());
	if (FAILED(hr) && pathAlt)
		hr = D3DReadFileToBlob(pathAlt, outBlob.ReleaseAndGetAddressOf());
	return SUCCEEDED(hr) && outBlob;
}

struct HiZCBuffer
{
	UINT SrcW = 0;
	UINT SrcH = 0;
	UINT DstW = 0;
	UINT DstH = 0;
	UINT _pad[60]{};
};
static_assert(sizeof(HiZCBuffer) == 256, "Hi-Z CB size");

UINT ComputeMipCount(UINT w, UINT h)
{
	UINT maxDim = (std::max)(w, h);
	UINT mips = 1u;
	while (maxDim > 1u)
	{
		maxDim /= 2u;
		++mips;
	}
	return mips;
}

UINT Subresource(UINT mip, UINT mipLevels)
{
	return D3D12CalcSubresource(mip, 0u, 0u, mipLevels, 1u);
}

} // namespace

bool HiZSystem::CreatePipelines(ID3D12Device* device)
{
	ComPtr<ID3DBlob> copyCs, reduceCs;
	if (!LoadHiZCsBlob(L"HiZ_CopyDepth_CS.cso", L"Shaders\\HiZ_CopyDepth_CS.cso", copyCs))
	{
		DebugLog("[HiZ] HiZ_CopyDepth_CS.cso not found\n");
		return false;
	}
	if (!LoadHiZCsBlob(L"HiZ_ReduceMin_CS.cso", L"Shaders\\HiZ_ReduceMin_CS.cso", reduceCs))
	{
		DebugLog("[HiZ] HiZ_ReduceMin_CS.cso not found\n");
		return false;
	}

	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	CD3DX12_DESCRIPTOR_RANGE uavRange;
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	CD3DX12_ROOT_PARAMETER rootParams[3] = {};
	rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
	rootParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
	rootParams[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = rootParams;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsBlob, rsErr;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr))
		return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_rootSignature.Get();

	psoDesc.CS = CD3DX12_SHADER_BYTECODE(copyCs.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_psoCopy.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	psoDesc.CS = CD3DX12_SHADER_BYTECODE(reduceCs.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_psoReduce.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	return true;
}

bool HiZSystem::Init(ID3D12Device* device, UINT width, UINT height, ID3D12Resource* depthBuffer)
{
	Shutdown();
	if (!device || !depthBuffer || width == 0 || height == 0)
		return false;

	m_w = width;
	m_h = height;
	m_mipCount = ComputeMipCount(width, height);
	m_depthResource = depthBuffer;

	auto heapPropDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_FLOAT,
		width,
		height,
		1u,
		m_mipCount,
		1u,
		0u,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	HRESULT hr = device->CreateCommittedResource(
		&heapPropDefault,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(m_hizPyramid.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	m_hizMipState.assign(m_mipCount, D3D12_RESOURCE_STATE_COMMON);

	const UINT numDesc = 1u + 2u * m_mipCount;
	D3D12_DESCRIPTOR_HEAP_DESC dh = {};
	dh.NumDescriptors = numDesc;
	dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(m_descHeap.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;

	m_descriptorStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_slotDepthSrv = 0;
	m_slotHiZSrvBase = 1;
	m_slotHiZUavBase = 1u + m_mipCount;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();

	// Depth SRV (R32_FLOAT view of R32_TYPELESS depth resource)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		srv.Texture2D.MostDetailedMip = 0;
		srv.Texture2D.ResourceMinLODClamp = 0.0f;
		device->CreateShaderResourceView(depthBuffer, &srv, cpu);
	}

	for (UINT mip = 0; mip < m_mipCount; ++mip)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE hSrv = cpu;
		hSrv.ptr += static_cast<SIZE_T>(m_slotHiZSrvBase + mip) * m_descriptorStride;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		srv.Texture2D.MostDetailedMip = mip;
		srv.Texture2D.ResourceMinLODClamp = 0.0f;
		device->CreateShaderResourceView(m_hizPyramid.Get(), &srv, hSrv);
	}

	for (UINT mip = 0; mip < m_mipCount; ++mip)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE hUav = cpu;
		hUav.ptr += static_cast<SIZE_T>(m_slotHiZUavBase + mip) * m_descriptorStride;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.Format = DXGI_FORMAT_R32_FLOAT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Texture2D.MipSlice = mip;
		device->CreateUnorderedAccessView(m_hizPyramid.Get(), nullptr, &uav, hUav);
	}

	auto heapPropUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resUpload = CD3DX12_RESOURCE_DESC::Buffer(256u);
	hr = device->CreateCommittedResource(
		&heapPropUpload,
		D3D12_HEAP_FLAG_NONE,
		&resUpload,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_paramBuffer.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	hr = m_paramBuffer->Map(0, nullptr, &m_paramMapped);
	if (FAILED(hr))
		return false;

	if (!CreatePipelines(device))
		return false;

	m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	m_valid = true;
	return true;
}

void HiZSystem::Shutdown()
{
	m_valid = false;
	m_psoCopy.Reset();
	m_psoReduce.Reset();
	m_rootSignature.Reset();
	if (m_paramBuffer && m_paramMapped)
	{
		m_paramBuffer->Unmap(0, nullptr);
		m_paramMapped = nullptr;
	}
	m_paramBuffer.Reset();
	m_descHeap.Reset();
	m_hizPyramid.Reset();
	m_hizMipState.clear();
	m_depthResource = nullptr;
	m_w = m_h = m_mipCount = 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE HiZSystem::CpuSrvDepth() const
{
	auto h = m_descHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_slotDepthSrv) * m_descriptorStride;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE HiZSystem::CpuSrvHiZ(UINT mip) const
{
	auto h = m_descHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_slotHiZSrvBase + mip) * m_descriptorStride;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE HiZSystem::CpuUavHiZ(UINT mip) const
{
	auto h = m_descHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_slotHiZUavBase + mip) * m_descriptorStride;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE HiZSystem::GpuSrvDepth() const
{
	auto h = m_descHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_slotDepthSrv) * m_descriptorStride;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE HiZSystem::GpuSrvHiZ(UINT mip) const
{
	auto h = m_descHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_slotHiZSrvBase + mip) * m_descriptorStride;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE HiZSystem::GpuUavHiZ(UINT mip) const
{
	auto h = m_descHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(m_slotHiZUavBase + mip) * m_descriptorStride;
	return h;
}

void HiZSystem::TransitionDepth(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, D3D12_RESOURCE_STATES to)
{
	if (!depth || m_depthState == to)
		return;
	const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(depth, m_depthState, to);
	cmd->ResourceBarrier(1, &barrier);
	m_depthState = to;
}

void HiZSystem::TransitionHiZMip(ID3D12GraphicsCommandList* cmd, UINT mip, D3D12_RESOURCE_STATES to)
{
	if (mip >= m_mipCount || m_hizMipState[mip] == to)
		return;
	const UINT sub = Subresource(mip, m_mipCount);
	const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		m_hizPyramid.Get(), m_hizMipState[mip], to, sub);
	cmd->ResourceBarrier(1, &barrier);
	m_hizMipState[mip] = to;
}

void HiZSystem::Build(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depthBuffer)
{
	if (!m_valid || !m_enabled || !cmd || !depthBuffer)
		return;

	ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetComputeRootSignature(m_rootSignature.Get());

	TransitionDepth(cmd, depthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	for (UINT m = 0; m < m_mipCount; ++m)
		TransitionHiZMip(cmd, m, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	HiZCBuffer* params = static_cast<HiZCBuffer*>(m_paramMapped);
	params->SrcW = m_w;
	params->SrcH = m_h;
	params->DstW = m_w;
	params->DstH = m_h;

	cmd->SetPipelineState(m_psoCopy.Get());
	cmd->SetComputeRootConstantBufferView(0, m_paramBuffer->GetGPUVirtualAddress());
	cmd->SetComputeRootDescriptorTable(1, GpuSrvDepth());
	cmd->SetComputeRootDescriptorTable(2, GpuUavHiZ(0));
	const UINT gx0 = (m_w + 7u) / 8u;
	const UINT gy0 = (m_h + 7u) / 8u;
	cmd->Dispatch(gx0, gy0, 1u);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = m_hizPyramid.Get();
	cmd->ResourceBarrier(1, &uavBarrier);

	cmd->SetPipelineState(m_psoReduce.Get());
	for (UINT srcMip = 0; srcMip + 1 < m_mipCount; ++srcMip)
	{
		const UINT dstMip = srcMip + 1;
		const UINT srcW = (std::max)(1u, m_w >> srcMip);
		const UINT srcH = (std::max)(1u, m_h >> srcMip);
		const UINT dstW = (std::max)(1u, m_w >> dstMip);
		const UINT dstH = (std::max)(1u, m_h >> dstMip);

		TransitionHiZMip(cmd, srcMip, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		TransitionHiZMip(cmd, dstMip, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		params->SrcW = srcW;
		params->SrcH = srcH;
		params->DstW = dstW;
		params->DstH = dstH;

		cmd->SetComputeRootConstantBufferView(0, m_paramBuffer->GetGPUVirtualAddress());
		cmd->SetComputeRootDescriptorTable(1, GpuSrvHiZ(srcMip));
		cmd->SetComputeRootDescriptorTable(2, GpuUavHiZ(dstMip));

		const UINT gx = (dstW + 7u) / 8u;
		const UINT gy = (dstH + 7u) / 8u;
		cmd->Dispatch(gx, gy, 1u);

		cmd->ResourceBarrier(1, &uavBarrier);
	}

	TransitionDepth(cmd, depthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);

	// 次フレームの Build 先頭で全ミップを UAV に戻すため、最終的に NPSRV にそろえる
	for (UINT m = 0; m < m_mipCount; ++m)
		TransitionHiZMip(cmd, m, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
