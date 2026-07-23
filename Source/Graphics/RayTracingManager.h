#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class DescriptorHeap;
struct Vertex;

/// Per-instance data for TLAS build.
struct RTInstance {
	DirectX::XMFLOAT3X4 transform;   // 3x4 row-major (D3D12_RAYTRACING_INSTANCE_DESC format)
	uint32_t blasIndex;                // index into m_blasList
	uint32_t instanceMask = 0xFF;
	uint32_t flags = 0;                // D3D12_RAYTRACING_INSTANCE_FLAGS
};

/// One triangle geometry (submesh) fed into a BLAS. Position must be at vertex offset 0.
struct RTGeometry {
	ID3D12Resource* vertexBuffer = nullptr;
	uint32_t vertexCount = 0;
	uint32_t vertexStride = 0;       // e.g. sizeof(Vertex) == 84
	ID3D12Resource* indexBuffer = nullptr;
	uint32_t indexCount = 0;
};

/// DXR 1.1 Ray Tracing Manager.
/// Builds BLAS per unique mesh, TLAS per frame from instance transforms.
/// Dispatches reflection rays for water surfaces.
class RayTracingManager
{
public:
	bool Init(ID3D12Device5* device, DescriptorHeap* heap, uint32_t screenW, uint32_t screenH);
	void Shutdown();

	/// Register one MODEL as a single BLAS built from N submesh geometries. Returns BLAS index.
	/// (One BLAS per model — NOT per submesh — keeps TLAS instance count ~= actor count.)
	uint32_t AddBLAS(ID3D12GraphicsCommandList4* cmd, const RTGeometry* geos, uint32_t geoCount);

	/// Build TLAS from instances. For the STATIC town this is called ONCE (PREFER_FAST_TRACE).
	void BuildTLAS(ID3D12GraphicsCommandList4* cmd,
	               const RTInstance* instances, uint32_t instanceCount);

	/// Free the transient BLAS/TLAS scratch buffers. Call ONCE after the one-time static build
	/// command list has been executed AND GPU-idle-waited (scratch must outlive the async build).
	void ReleaseBuildScratch() { m_buildScratch.clear(); m_tlasScratch.Reset(); }

	/// TLAS GPU VA for inline RayQuery (RaytracingAccelerationStructure SRV via root SRV).
	D3D12_GPU_VIRTUAL_ADDRESS GetTlasGpuVA() const {
		return m_tlasBuffer ? m_tlasBuffer->GetGPUVirtualAddress() : 0;
	}
	uint32_t GetInstanceCount() const { return m_tlasInstanceCount; }

	/// Dispatch reflection rays for water pixels.
	/// Output: half-resolution reflection texture.
	void DispatchWaterReflection(ID3D12GraphicsCommandList4* cmd,
	                             ID3D12Resource* depthBuffer,
	                             const DirectX::XMMATRIX& invViewProj,
	                             const DirectX::XMFLOAT3& cameraPos,
	                             float waterSurfaceY);

	/// F1 検証: primary ray のヒット距離 vs ラスタ深度を色分けして HDR RTV へフルスクリーン描画。
	/// 緑=一致(TLAS正), 赤=不一致, 青=RT取りこぼし。inline RayQuery(fullscreen PS)。
	void RenderDebugPrimary(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
	                        ID3D12Resource* depthResource,
	                        const DirectX::XMMATRIX& invViewProj, const DirectX::XMFLOAT3& cameraPos);

	/// Bindless heap index of the reflection output texture (SRV).
	uint32_t GetReflectionSrvIdx() const { return m_reflectionSrvIdx; }
	ID3D12Resource* GetReflectionTexture() const { return m_reflectionTexture.Get(); }

	bool IsValid() const { return m_valid; }

private:
	bool CreateReflectionResources(ID3D12Device5* device, DescriptorHeap* heap);
	bool CreateRTPipeline(ID3D12Device5* device);

	bool m_valid = false;
	uint32_t m_screenW = 0, m_screenH = 0;

	// BLAS list (scratch NOT retained here — see m_buildScratch)
	struct BLASEntry {
		ComPtr<ID3D12Resource> blasBuffer;
	};
	std::vector<BLASEntry> m_blasList;

	// Transient scratch for the one-time static build (BLAS + TLAS). Held until GPU-idle,
	// then freed via ReleaseBuildScratch() — fixes the old per-BLAS permanent-scratch VRAM leak.
	std::vector<ComPtr<ID3D12Resource>> m_buildScratch;

	// TLAS
	ComPtr<ID3D12Resource> m_tlasBuffer;
	ComPtr<ID3D12Resource> m_tlasScratch;
	ComPtr<ID3D12Resource> m_instanceDescBuffer; // upload heap for D3D12_RAYTRACING_INSTANCE_DESC
	uint32_t m_tlasMaxInstances = 0;
	uint32_t m_tlasSrvIdx = UINT32_MAX;
	uint32_t m_tlasInstanceCount = 0;

	// Reflection output (half-res RGBA16F)
	ComPtr<ID3D12Resource> m_reflectionTexture;
	uint32_t m_reflectionSrvIdx = UINT32_MAX;
	uint32_t m_reflectionUavIdx = UINT32_MAX;

	// RT Pipeline State Object
	ComPtr<ID3D12StateObject> m_rtPso;
	ComPtr<ID3D12StateObjectProperties> m_rtPsoProperties;

	// Shader table
	ComPtr<ID3D12Resource> m_shaderTable;
	D3D12_GPU_VIRTUAL_ADDRESS m_rayGenRecord = 0;
	D3D12_GPU_VIRTUAL_ADDRESS m_missRecord = 0;
	D3D12_GPU_VIRTUAL_ADDRESS m_hitGroupRecord = 0;
	uint32_t m_shaderRecordSize = 0;

	// Global root signature for RT shaders (dormant DispatchRays path)
	ComPtr<ID3D12RootSignature> m_globalRootSig;

	// F1 debug: primary-ray hit-distance visualization (fullscreen PS + inline RayQuery)
	ComPtr<ID3D12RootSignature>  m_debugRootSig;
	ComPtr<ID3D12PipelineState>  m_debugPso;
	ComPtr<ID3D12DescriptorHeap> m_debugHeap;   // [0] = scene depth SRV
	bool CreateDebugPipeline(ID3D12Device5* device);

	// Constants
	struct RTConstants {
		DirectX::XMMATRIX InvViewProj;
		DirectX::XMFLOAT4 CameraPos;
		float waterSurfaceY;
		uint32_t outputWidth;
		uint32_t outputHeight;
		uint32_t _pad;
	};
	ComPtr<ID3D12Resource> m_constantsCB;
	void* m_constantsMapped = nullptr;
};
