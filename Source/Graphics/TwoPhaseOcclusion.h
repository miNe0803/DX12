#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <cstdint>

class DescriptorHeap;
class HiZSystem;

/// Manages GPU buffers for Two-Phase Occlusion Culling.
///
/// Phase 1: Cull against previous frame's Hi-Z → draw visible → build new Hi-Z
/// Phase 2: Re-test Phase 1 culled items against new Hi-Z → draw newly visible
///
/// This eliminates the "false occlusion elimination" (誤遮蔽で全滅) problem
/// because objects only stay culled if they fail BOTH tests.
class TwoPhaseOcclusion
{
public:
	static constexpr uint32_t kMaxCulledMeshlets = 262144; // should cover any scene

	bool Init(ID3D12Device* device, DescriptorHeap* heap);
	void Shutdown();

	/// Reset the culled list counter to 0. Call once per frame before Phase 1.
	void ResetCulledList(ID3D12GraphicsCommandList* cmd);

	/// Get the constants to bind as b3 s0 for the AS.
	struct TwoPhaseConstants {
		uint32_t phase;            // 0=Phase1, 1=Phase2
		uint32_t hizWidth;
		uint32_t hizHeight;
		uint32_t hizMipCount;
		float    hizNearDisableDist;
		float    hizDepthBias;
		float    hizMaxPixelRadius;
		uint32_t totalMeshlets;
	};
	static_assert(sizeof(TwoPhaseConstants) == 32, "TwoPhaseConstants size");

	/// Fill constants for a given phase. Caller binds this as root CBV or root constants.
	TwoPhaseConstants MakeConstants(uint32_t phase, const HiZSystem* hiz, uint32_t totalMeshlets) const;

	/// GPU address of the upload CB (mapped, updated each call to WriteConstants).
	void WriteConstants(const TwoPhaseConstants& c);
	D3D12_GPU_VIRTUAL_ADDRESS GetConstantsCBAddress() const;

	/// Culled list buffer (UAV for Phase 1 write, SRV for Phase 2 read).
	ID3D12Resource* GetCulledListResource() const { return m_culledListBuffer.Get(); }
	ID3D12Resource* GetCulledCountResource() const { return m_culledCountBuffer.Get(); }

	/// Bindless heap indices for the culled list UAVs.
	uint32_t GetCulledListUavHeapIdx() const { return m_culledListUavIdx; }
	uint32_t GetCulledCountUavHeapIdx() const { return m_culledCountUavIdx; }

	/// Read back the culled count from GPU (for Phase 2 dispatch size).
	/// Only valid after Phase 1 completes + readback copy + fence wait.
	/// For simplicity, we use a conservative max dispatch in Phase 2.
	/// In production, a GPU indirect dispatch would be used.
	uint32_t GetMaxPhase2DispatchCount() const { return kMaxCulledMeshlets; }

	/// Transition helpers
	void TransitionCulledListForWrite(ID3D12GraphicsCommandList* cmd);
	void TransitionCulledListForRead(ID3D12GraphicsCommandList* cmd);

private:
	bool m_valid = false;

	// Culled meshlet index list (RWByteAddressBuffer, Phase 1 writes meshlet IDs)
	ComPtr<ID3D12Resource> m_culledListBuffer;
	uint32_t m_culledListUavIdx = UINT32_MAX;
	D3D12_RESOURCE_STATES m_culledListState = D3D12_RESOURCE_STATE_COMMON;

	// Atomic counter for culled list (RWByteAddressBuffer, single uint32)
	ComPtr<ID3D12Resource> m_culledCountBuffer;
	uint32_t m_culledCountUavIdx = UINT32_MAX;
	D3D12_RESOURCE_STATES m_culledCountState = D3D12_RESOURCE_STATE_COMMON;

	// Upload buffer for resetting counter to 0
	ComPtr<ID3D12Resource> m_counterResetUpload;

	// Constants upload CB (256-byte aligned)
	ComPtr<ID3D12Resource> m_constantsCB;
	void* m_constantsMapped = nullptr;
};
