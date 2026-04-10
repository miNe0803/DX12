#include "TwoPhaseOcclusion.h"
#include "HiZSystem.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <cstring>
#include <cstdio>

bool TwoPhaseOcclusion::Init(ID3D12Device* device, DescriptorHeap* heap)
{
	if (!device || !heap) return false;

	const UINT64 listByteSize = static_cast<UINT64>(kMaxCulledMeshlets) * sizeof(uint32_t);
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto uploadProp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	// Culled list buffer (RWByteAddressBuffer)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(listByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		auto hr = device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_culledListBuffer.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) { printf("TwoPhase: culled list buffer failed\n"); return false; }
		m_culledListState = D3D12_RESOURCE_STATE_COMMON;

		m_culledListUavIdx = heap->AllocateIndex();
		if (m_culledListUavIdx == UINT32_MAX) return false;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = kMaxCulledMeshlets;
		uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		heap->CreateUAVAt(m_culledListUavIdx, m_culledListBuffer.Get(), uav);
	}

	// Culled count buffer (RWByteAddressBuffer, single uint32 counter)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		auto hr = device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_culledCountBuffer.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) { printf("TwoPhase: culled count buffer failed\n"); return false; }
		m_culledCountState = D3D12_RESOURCE_STATE_COMMON;

		m_culledCountUavIdx = heap->AllocateIndex();
		if (m_culledCountUavIdx == UINT32_MAX) return false;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		heap->CreateUAVAt(m_culledCountUavIdx, m_culledCountBuffer.Get(), uav);
	}

	// Counter reset upload (4 bytes of zeros)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));
		auto hr = device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_counterResetUpload.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		void* mapped = nullptr;
		D3D12_RANGE readRange{ 0, 0 };
		m_counterResetUpload->Map(0, &readRange, &mapped);
		memset(mapped, 0, sizeof(uint32_t));
		m_counterResetUpload->Unmap(0, nullptr);
	}

	// Constants CB (256-byte aligned upload)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
		auto hr = device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_constantsCB.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return false;
		D3D12_RANGE readRange{ 0, 0 };
		m_constantsCB->Map(0, &readRange, &m_constantsMapped);
	}

	m_valid = true;
	printf("TwoPhaseOcclusion::Init: OK (max %u culled meshlets)\n", kMaxCulledMeshlets);
	return true;
}

void TwoPhaseOcclusion::Shutdown()
{
	if (m_constantsCB && m_constantsMapped)
	{
		m_constantsCB->Unmap(0, nullptr);
		m_constantsMapped = nullptr;
	}
	m_culledListBuffer.Reset();
	m_culledCountBuffer.Reset();
	m_counterResetUpload.Reset();
	m_constantsCB.Reset();
	m_valid = false;
}

void TwoPhaseOcclusion::ResetCulledList(ID3D12GraphicsCommandList* cmd)
{
	if (!m_valid || !cmd) return;

	// Transition counter to COPY_DEST, copy 0, transition back to UAV
	if (m_culledCountState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_culledCountBuffer.Get(), m_culledCountState, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->ResourceBarrier(1, &barrier);
		m_culledCountState = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	cmd->CopyBufferRegion(m_culledCountBuffer.Get(), 0, m_counterResetUpload.Get(), 0, sizeof(uint32_t));

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		m_culledCountBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->ResourceBarrier(1, &barrier);
	m_culledCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

TwoPhaseOcclusion::TwoPhaseConstants TwoPhaseOcclusion::MakeConstants(
	uint32_t phase, const HiZSystem* hiz, uint32_t totalMeshlets) const
{
	TwoPhaseConstants c{};
	c.phase = phase;
	c.totalMeshlets = totalMeshlets;

	if (hiz && hiz->IsValid() && hiz->GetEnabled())
	{
		c.hizWidth  = hiz->GetWidth();
		c.hizHeight = hiz->GetHeight();
		c.hizMipCount = hiz->GetMipCount();
	}
	else
	{
		c.hizWidth = c.hizHeight = 0;
		c.hizMipCount = 0; // disables Hi-Z test in shader
	}

	c.hizNearDisableDist = 150.0f;
	c.hizDepthBias       = 0.01f;
	c.hizMaxPixelRadius  = 96.0f;
	return c;
}

void TwoPhaseOcclusion::WriteConstants(const TwoPhaseConstants& c)
{
	if (m_constantsMapped)
		memcpy(m_constantsMapped, &c, sizeof(c));
}

D3D12_GPU_VIRTUAL_ADDRESS TwoPhaseOcclusion::GetConstantsCBAddress() const
{
	return m_constantsCB ? m_constantsCB->GetGPUVirtualAddress() : 0;
}

void TwoPhaseOcclusion::TransitionCulledListForWrite(ID3D12GraphicsCommandList* cmd)
{
	if (m_culledListState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_culledListBuffer.Get(), m_culledListState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &barrier);
		m_culledListState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
}

void TwoPhaseOcclusion::TransitionCulledListForRead(ID3D12GraphicsCommandList* cmd)
{
	D3D12_RESOURCE_STATES readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	if (m_culledListState != readState)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_culledListBuffer.Get(), m_culledListState, readState);
		cmd->ResourceBarrier(1, &barrier);
		m_culledListState = readState;
	}
	// Also transition counter to read
	if (m_culledCountState != readState)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_culledCountBuffer.Get(), m_culledCountState, readState);
		cmd->ResourceBarrier(1, &barrier);
		m_culledCountState = readState;
	}
}
