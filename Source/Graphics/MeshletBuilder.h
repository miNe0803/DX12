#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <vector>
#include "ComPtr.h"
#include <d3d12.h>

class DescriptorHeap;
struct Vertex;

/// Per-meshlet descriptor — offsets into the global vertex-index and primitive-index arrays.
struct Meshlet {
	uint32_t vertexOffset;      // first entry in uniqueVertexIndices[]
	uint32_t vertexCount;       // # unique vertices in this meshlet (max 64)
	uint32_t primitiveOffset;   // first entry in primitiveIndices[] (byte offset / 4)
	uint32_t primitiveCount;    // # triangles (max 124)
};

/// Culling data for one meshlet (local space).
struct MeshletBounds {
	DirectX::XMFLOAT3 center;
	float radius;
	DirectX::XMFLOAT3 coneAxis;   // average normal direction
	float coneCutoff;              // dot threshold for backface cone (negative = wider)
};
static_assert(sizeof(MeshletBounds) == 32, "MeshletBounds must be 32 bytes");

/// CPU-side meshlet data for one mesh (generated at load time).
struct MeshletMeshCPU {
	std::vector<Meshlet>       meshlets;
	std::vector<MeshletBounds> bounds;
	std::vector<uint32_t>      uniqueVertexIndices;   // meshlet-local → global vertex index
	std::vector<uint32_t>      primitiveIndices;       // packed: 3 × 10-bit indices per uint32
	uint32_t                   materialIndex = 0;
};

/// GPU buffers for all meshlets in the scene. Registered in the bindless heap.
struct MeshletGpuBuffers {
	ComPtr<ID3D12Resource> meshletBuffer;         // StructuredBuffer<Meshlet>
	ComPtr<ID3D12Resource> boundsBuffer;           // StructuredBuffer<MeshletBounds>
	ComPtr<ID3D12Resource> uniqueVertexIdxBuffer;  // ByteAddressBuffer (uint32 per entry)
	ComPtr<ID3D12Resource> primitiveIdxBuffer;     // ByteAddressBuffer (packed uint32)
	ComPtr<ID3D12Resource> vertexBuffer;           // StructuredBuffer<Vertex>

	uint32_t meshletBufferSrvIdx     = UINT32_MAX;
	uint32_t boundsBufferSrvIdx      = UINT32_MAX;
	uint32_t uniqueVertexIdxSrvIdx   = UINT32_MAX;
	uint32_t primitiveIdxSrvIdx      = UINT32_MAX;
	uint32_t vertexBufferSrvIdx      = UINT32_MAX;

	uint32_t totalMeshlets = 0;
	uint32_t totalVertices = 0;
};

namespace MeshletBuilder
{
	/// Max vertices / primitives per meshlet (NVIDIA-optimal).
	static constexpr uint32_t kMaxVerticesPerMeshlet   = 64;
	static constexpr uint32_t kMaxPrimitivesPerMeshlet = 124;

	/// Build meshlets from a triangle mesh (CPU). Simple greedy algorithm.
	MeshletMeshCPU Build(const Vertex* vertices, uint32_t vertexCount,
	                     const uint32_t* indices, uint32_t indexCount);

	/// Upload a collection of MeshletMeshCPU to the GPU and register in the bindless heap.
	/// All meshlets are concatenated into a single set of global buffers.
	bool UploadToGpu(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* cmd,
		DescriptorHeap* heap,
		const Vertex* allVertices, uint32_t totalVertexCount,
		const std::vector<MeshletMeshCPU>& meshes,
		MeshletGpuBuffers& outGpu);

	/// Pack 3 triangle vertex indices (each 0..63) into one uint32.
	inline uint32_t PackPrimitive(uint32_t i0, uint32_t i1, uint32_t i2)
	{
		return (i0 & 0x3FF) | ((i1 & 0x3FF) << 10) | ((i2 & 0x3FF) << 20);
	}
}
