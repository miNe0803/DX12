#pragma once
#include "ComPtr.h"

struct ID3D12RootSignature;

/// Root signature variants.
enum class RootSigType {
	PbrLegacy,      // Legacy PBR with descriptor tables (backward compat during transition)
	TerrainLegacy,  // Legacy terrain with descriptor tables
	Bindless,       // SM6.6 bindless (CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)
	TerrainBindless, // Terrain bindless variant
};

class RootSignature
{
public:
	// Legacy constructors (kept for backward compatibility during transition)
	explicit RootSignature(bool forTerrain = false);
	// Bindless constructor
	explicit RootSignature(RootSigType type);

	bool IsValid();
	ID3D12RootSignature* Get();

	// Bindless root parameter indices
	static constexpr UINT kBindlessParam_SceneCBV     = 0; // b0 s0: SceneConstants
	static constexpr UINT kBindlessParam_ShadowCBV    = 1; // b1 s0: ShadowConstants
	static constexpr UINT kBindlessParam_DrawConstants = 2; // b2 s0: 4x32-bit root constants
	static constexpr UINT kBindlessParam_MeshletSRV   = 3; // t0 s1: meshlet/tree extra data

private:
	void CreateLegacy(bool forTerrain);
	void CreateBindless(bool forTerrain);

	bool m_IsValid = false;
	ComPtr<ID3D12RootSignature> m_pRootSignature = nullptr;
};
