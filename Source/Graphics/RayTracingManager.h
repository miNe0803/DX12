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

/// DXR 1.1 Ray Tracing Manager.
/// Builds BLAS per unique mesh, TLAS per frame from instance transforms.
/// Dispatches reflection rays for water surfaces.
class RayTracingManager
{
public:
	bool Init(ID3D12Device5* device, DescriptorHeap* heap, uint32_t screenW, uint32_t screenH);
	void Shutdown();

	/// Register a unique mesh as a BLAS. Returns BLAS index.
	uint32_t AddBLAS(ID3D12GraphicsCommandList4* cmd,
	                 ID3D12Resource* vertexBuffer, uint32_t vertexCount, uint32_t vertexStride,
	                 ID3D12Resource* indexBuffer, uint32_t indexCount);

	/// Rebuild TLAS from current frame's instances. Call once per frame.
	void BuildTLAS(ID3D12GraphicsCommandList4* cmd,
	               const RTInstance* instances, uint32_t instanceCount);

	/// Dispatch reflection rays for water pixels.
	/// Output: half-resolution reflection texture.
	void DispatchWaterReflection(ID3D12GraphicsCommandList4* cmd,
	                             ID3D12Resource* depthBuffer,
	                             const DirectX::XMMATRIX& invViewProj,
	                             const DirectX::XMFLOAT3& cameraPos,
	                             float waterSurfaceY);

	/// Bindless heap index of the reflection output texture (SRV).
	uint32_t GetReflectionSrvIdx() const { return m_reflectionSrvIdx; }
	ID3D12Resource* GetReflectionTexture() const { return m_reflectionTexture.Get(); }

	bool IsValid() const { return m_valid; }

private:
	bool CreateReflectionResources(ID3D12Device5* device, DescriptorHeap* heap);
	bool CreateRTPipeline(ID3D12Device5* device);

	bool m_valid = false;
	uint32_t m_screenW = 0, m_screenH = 0;

	// BLAS list
	struct BLASEntry {
		ComPtr<ID3D12Resource> blasBuffer;
		ComPtr<ID3D12Resource> scratchBuffer;
	};
	std::vector<BLASEntry> m_blasList;

	// TLAS
	ComPtr<ID3D12Resource> m_tlasBuffer;
	ComPtr<ID3D12Resource> m_tlasScratch;
	ComPtr<ID3D12Resource> m_instanceDescBuffer; // upload heap for D3D12_RAYTRACING_INSTANCE_DESC
	uint32_t m_tlasMaxInstances = 0;
	uint32_t m_tlasSrvIdx = UINT32_MAX;

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

	// Global root signature for RT shaders
	ComPtr<ID3D12RootSignature> m_globalRootSig;

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
