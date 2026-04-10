#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>
#include "ComPtr.h"

class DescriptorHeap;

/// GPU-side material data for SM6.6 bindless rendering.
/// Layout must exactly match the HLSL MaterialData struct in Bindless.hlsli.
struct GpuMaterialData {
	// --- Texture heap indices (ResourceDescriptorHeap[idx]) ---
	uint32_t albedoTexIdx    = UINT32_MAX;
	uint32_t normalTexIdx    = UINT32_MAX;
	uint32_t metallicTexIdx  = UINT32_MAX;
	uint32_t roughnessTexIdx = UINT32_MAX;
	uint32_t rampTexIdx      = UINT32_MAX; // NPR ramp (UINT32_MAX = unused)
	uint32_t sphereMapTexIdx = UINT32_MAX; // NPR PMX sphere map
	uint32_t shadowMapArrayIdx = UINT32_MAX;
	uint32_t prefilterEnvIdx = UINT32_MAX; // IBL
	uint32_t irradianceMapIdx = UINT32_MAX; // IBL
	uint32_t brdfLutIdx      = UINT32_MAX; // IBL

	// --- Shading model ---
	uint32_t shadingModel = 0; // 0=PBR, 1=NPR, 2=Water

	// --- NPR tuning (used only when shadingModel==1) ---
	float celSharpness        = 0.58f;
	float rimPower            = 5.0f;
	float rimStrength         = 0.3f;
	float nprExposureOverride = 1.0f;
	uint32_t sphereMode       = 0; // PMX: 0=off, 1=mul, 2+=add
	float celVertexBlend      = 0.52f;
	float nprOpacity          = 1.0f;

	float _pad[2] = {};
};
static_assert(sizeof(GpuMaterialData) == 80, "GpuMaterialData must be 80 bytes for StructuredBuffer stride.");

/// Manages a GPU StructuredBuffer<GpuMaterialData> for bindless material lookup.
class MaterialManager {
public:
	/// max materials (can grow if needed via realloc)
	static constexpr uint32_t kMaxMaterials = 4096;

	/// Initialize the manager: creates the GPU buffer and registers it in the bindless heap.
	/// Returns false on failure.
	bool Init(DescriptorHeap* heap);

	/// Register a material and return its index into the StructuredBuffer.
	uint32_t RegisterMaterial(const GpuMaterialData& mat);

	/// Upload all pending material changes to the GPU buffer.
	/// Call once per frame before rendering (or after all materials are registered).
	void UploadToGpu(ID3D12GraphicsCommandList* cmd);

	/// Heap index of the StructuredBuffer<GpuMaterialData> SRV.
	uint32_t GetBufferSrvHeapIndex() const { return m_bufferSrvHeapIndex; }

	/// Number of registered materials.
	uint32_t Count() const { return static_cast<uint32_t>(m_materials.size()); }

private:
	std::vector<GpuMaterialData> m_materials;

	ComPtr<ID3D12Resource> m_defaultBuffer;
	ComPtr<ID3D12Resource> m_uploadBuffer;
	uint32_t m_bufferSrvHeapIndex = UINT32_MAX;
	bool m_dirty = false;
	uint32_t m_gpuCapacity = 0;
};
