#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class DescriptorHeap;

/// GPU light data for Forward+ (must match HLSL LightData in ClusterShading.hlsli).
struct GpuLight {
	DirectX::XMFLOAT3 position;
	float range;                 // attenuation radius
	DirectX::XMFLOAT3 color;
	float intensity;
	DirectX::XMFLOAT3 direction; // spot lights only
	float spotAngleCos;          // cos(outerConeAngle), 0 for point lights
	uint32_t type;               // 0=point, 1=spot
	float spotInnerCos;          // cos(innerConeAngle)
	float _pad[2];
};
static_assert(sizeof(GpuLight) == 64, "GpuLight must be 64 bytes");

/// Forward+ Clustered Shading system.
/// Divides the frustum into a 3D grid of clusters and assigns lights per cluster.
class ClusterGrid
{
public:
	static constexpr uint32_t kTileSize      = 16;  // pixels per tile in X/Y
	static constexpr uint32_t kDepthSlices   = 24;  // logarithmic depth slices
	static constexpr uint32_t kMaxLights     = 1024;
	static constexpr uint32_t kMaxLightsPerCluster = 128;

	bool Init(ID3D12Device* device, DescriptorHeap* heap, uint32_t screenW, uint32_t screenH);
	void Shutdown();

	/// Update the light buffer with current frame's lights. Call before AssignLights.
	void UpdateLights(ID3D12GraphicsCommandList* cmd, const GpuLight* lights, uint32_t count);

	/// Dispatch the cluster light assignment compute shader.
	void AssignLights(ID3D12GraphicsCommandList* cmd,
	                  const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
	                  float nearZ, float farZ);

	/// Bindless heap indices for shader access.
	uint32_t GetLightBufferSrvIdx() const { return m_lightBufferSrvIdx; }
	uint32_t GetClusterDataSrvIdx() const { return m_clusterDataSrvIdx; }
	uint32_t GetLightIndexListSrvIdx() const { return m_lightIndexListSrvIdx; }

	uint32_t GetTileCountX() const { return m_tileCountX; }
	uint32_t GetTileCountY() const { return m_tileCountY; }
	uint32_t GetActiveLightCount() const { return m_activeLightCount; }

	/// Log-depth slice parameters for the pixel shader.
	float GetLogScale() const { return m_logScale; }
	float GetLogBias() const { return m_logBias; }

private:
	bool CreateBuffers(ID3D12Device* device, DescriptorHeap* heap);
	bool CreatePipeline(ID3D12Device* device);

	bool m_valid = false;

	uint32_t m_screenW = 0, m_screenH = 0;
	uint32_t m_tileCountX = 0, m_tileCountY = 0;
	uint32_t m_totalClusters = 0;
	uint32_t m_activeLightCount = 0;
	float m_logScale = 0.0f, m_logBias = 0.0f;

	// Light buffer (StructuredBuffer<GpuLight>)
	ComPtr<ID3D12Resource> m_lightBuffer;
	ComPtr<ID3D12Resource> m_lightUpload;
	uint32_t m_lightBufferSrvIdx = UINT32_MAX;

	// Cluster data: per-cluster (offset, count) → RWStructuredBuffer<uint2>
	ComPtr<ID3D12Resource> m_clusterDataBuffer;
	uint32_t m_clusterDataSrvIdx = UINT32_MAX;
	uint32_t m_clusterDataUavIdx = UINT32_MAX;

	// Global light index list: flat array of light indices
	ComPtr<ID3D12Resource> m_lightIndexListBuffer;
	uint32_t m_lightIndexListSrvIdx = UINT32_MAX;
	uint32_t m_lightIndexListUavIdx = UINT32_MAX;

	// Global atomic counter for light index list allocation
	ComPtr<ID3D12Resource> m_globalCounterBuffer;
	uint32_t m_globalCounterUavIdx = UINT32_MAX;
	ComPtr<ID3D12Resource> m_counterResetUpload;

	// Compute pipeline
	ComPtr<ID3D12RootSignature> m_rootSig;
	ComPtr<ID3D12PipelineState> m_pso;

	// Constants
	struct ClusterAssignCB {
		DirectX::XMMATRIX InvProj;
		DirectX::XMMATRIX View;
		float nearZ, farZ;
		float logScale, logBias;
		uint32_t tileCountX, tileCountY, depthSlices, lightCount;
		uint32_t screenW, screenH;
		uint32_t _pad[2];
	};
	static_assert(sizeof(ClusterAssignCB) % 16 == 0);
	ComPtr<ID3D12Resource> m_cbUpload;
	void* m_cbMapped = nullptr;
};
