#include "MeshletBuilder.h"
#include "../../DescriptorHeap.h"
#include "../../SharedStruct.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <DirectXMath.h>
#include <algorithm>
#include <unordered_set>
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace DirectX;

// ============================================================
// Greedy meshlet builder
// ============================================================

MeshletMeshCPU MeshletBuilder::Build(
	const Vertex* vertices, uint32_t vertexCount,
	const uint32_t* indices, uint32_t indexCount)
{
	MeshletMeshCPU result;
	if (!vertices || !indices || indexCount == 0) return result;

	const uint32_t triCount = indexCount / 3;

	// Current meshlet being built
	std::vector<uint32_t> meshletVertices;   // unique vertex indices (global)
	std::vector<uint32_t> meshletPrimitives; // packed triangle indices (meshlet-local)
	meshletVertices.reserve(kMaxVerticesPerMeshlet);
	meshletPrimitives.reserve(kMaxPrimitivesPerMeshlet);

	// Fast lookup: global vertex index → meshlet-local index
	std::unordered_map<uint32_t, uint32_t> vertexMap;
	vertexMap.reserve(kMaxVerticesPerMeshlet * 2);

	auto FlushMeshlet = [&]()
	{
		if (meshletPrimitives.empty()) return;

		Meshlet m{};
		m.vertexOffset    = static_cast<uint32_t>(result.uniqueVertexIndices.size());
		m.vertexCount     = static_cast<uint32_t>(meshletVertices.size());
		m.primitiveOffset = static_cast<uint32_t>(result.primitiveIndices.size());
		m.primitiveCount  = static_cast<uint32_t>(meshletPrimitives.size());

		result.meshlets.push_back(m);
		result.uniqueVertexIndices.insert(result.uniqueVertexIndices.end(),
			meshletVertices.begin(), meshletVertices.end());
		result.primitiveIndices.insert(result.primitiveIndices.end(),
			meshletPrimitives.begin(), meshletPrimitives.end());

		// Compute bounds
		XMVECTOR minV = XMVectorReplicate(FLT_MAX);
		XMVECTOR maxV = XMVectorReplicate(-FLT_MAX);
		XMVECTOR normalSum = XMVectorZero();

		for (uint32_t vi : meshletVertices)
		{
			XMVECTOR pos = XMLoadFloat3(&vertices[vi].Position);
			minV = XMVectorMin(minV, pos);
			maxV = XMVectorMax(maxV, pos);
			normalSum = XMVectorAdd(normalSum, XMLoadFloat3(&vertices[vi].Normal));
		}

		XMVECTOR center = (minV + maxV) * 0.5f;
		float radius = 0.0f;
		for (uint32_t vi : meshletVertices)
		{
			float d = XMVectorGetX(XMVector3Length(XMLoadFloat3(&vertices[vi].Position) - center));
			radius = std::max(radius, d);
		}

		XMVECTOR avgNormal = XMVector3Normalize(normalSum);
		float minDot = 1.0f;
		for (uint32_t vi : meshletVertices)
		{
			float d = XMVectorGetX(XMVector3Dot(XMVector3Normalize(XMLoadFloat3(&vertices[vi].Normal)), avgNormal));
			minDot = std::min(minDot, d);
		}

		MeshletBounds bounds{};
		XMStoreFloat3(&bounds.center, center);
		bounds.radius = radius;
		XMStoreFloat3(&bounds.coneAxis, avgNormal);
		bounds.coneCutoff = minDot; // if dot(viewDir, coneAxis) < -coneCutoff → all backfacing
		result.bounds.push_back(bounds);

		meshletVertices.clear();
		meshletPrimitives.clear();
		vertexMap.clear();
	};

	for (uint32_t tri = 0; tri < triCount; ++tri)
	{
		uint32_t i0 = indices[tri * 3 + 0];
		uint32_t i1 = indices[tri * 3 + 1];
		uint32_t i2 = indices[tri * 3 + 2];

		// Count how many new vertices this triangle would add
		uint32_t newVerts = 0;
		if (vertexMap.find(i0) == vertexMap.end()) ++newVerts;
		if (vertexMap.find(i1) == vertexMap.end()) ++newVerts;
		if (vertexMap.find(i2) == vertexMap.end()) ++newVerts;

		// Check if adding this triangle would exceed limits
		bool vertexOverflow = (meshletVertices.size() + newVerts) > kMaxVerticesPerMeshlet;
		bool primOverflow   = (meshletPrimitives.size() + 1) > kMaxPrimitivesPerMeshlet;

		if (vertexOverflow || primOverflow)
		{
			FlushMeshlet();
		}

		// Add vertices (get or assign meshlet-local indices)
		auto GetOrAdd = [&](uint32_t globalIdx) -> uint32_t
		{
			auto it = vertexMap.find(globalIdx);
			if (it != vertexMap.end()) return it->second;
			uint32_t local = static_cast<uint32_t>(meshletVertices.size());
			vertexMap[globalIdx] = local;
			meshletVertices.push_back(globalIdx);
			return local;
		};

		uint32_t l0 = GetOrAdd(i0);
		uint32_t l1 = GetOrAdd(i1);
		uint32_t l2 = GetOrAdd(i2);

		meshletPrimitives.push_back(PackPrimitive(l0, l1, l2));
	}

	FlushMeshlet();

	printf("MeshletBuilder: %u tris -> %zu meshlets (avg %.1f tris/meshlet)\n",
		triCount, result.meshlets.size(),
		result.meshlets.empty() ? 0.0 : static_cast<double>(triCount) / result.meshlets.size());

	return result;
}

// ============================================================
// GPU upload
// ============================================================

static ComPtr<ID3D12Resource> CreateDefaultBuffer(
	ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
	const void* data, UINT64 byteSize,
	ComPtr<ID3D12Resource>& uploadBuffer)
{
	ComPtr<ID3D12Resource> defaultBuf;
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
	device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&defaultBuf));

	auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

	void* mapped = nullptr;
	D3D12_RANGE readRange{ 0, 0 };
	uploadBuffer->Map(0, &readRange, &mapped);
	memcpy(mapped, data, static_cast<size_t>(byteSize));
	uploadBuffer->Unmap(0, nullptr);

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuf.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->ResourceBarrier(1, &barrier);
	cmd->CopyBufferRegion(defaultBuf.Get(), 0, uploadBuffer.Get(), 0, byteSize);
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuf.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &barrier);

	return defaultBuf;
}

static uint32_t RegisterStructuredBufferSRV(
	DescriptorHeap* heap, ID3D12Resource* resource,
	uint32_t numElements, uint32_t stride)
{
	uint32_t idx = heap->AllocateIndex();
	if (idx == UINT32_MAX) return idx;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.FirstElement = 0;
	srv.Buffer.NumElements = numElements;
	srv.Buffer.StructureByteStride = stride;
	heap->CreateSRVAt(idx, resource, srv);
	return idx;
}

static uint32_t RegisterRawBufferSRV(
	DescriptorHeap* heap, ID3D12Resource* resource, uint32_t byteSize)
{
	uint32_t idx = heap->AllocateIndex();
	if (idx == UINT32_MAX) return idx;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R32_TYPELESS;
	srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.FirstElement = 0;
	srv.Buffer.NumElements = byteSize / 4;
	srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	heap->CreateSRVAt(idx, resource, srv);
	return idx;
}

bool MeshletBuilder::UploadToGpu(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmd,
	DescriptorHeap* heap,
	const Vertex* allVertices, uint32_t totalVertexCount,
	const std::vector<MeshletMeshCPU>& meshes,
	MeshletGpuBuffers& out)
{
	// Concatenate all meshlet data
	std::vector<Meshlet>       allMeshlets;
	std::vector<MeshletBounds> allBounds;
	std::vector<uint32_t>      allUniqueVtx;
	std::vector<uint32_t>      allPrimIdx;

	uint32_t meshletOffset = 0;
	uint32_t uniqueVtxOffset = 0;
	uint32_t primIdxOffset = 0;

	for (const auto& mesh : meshes)
	{
		for (size_t i = 0; i < mesh.meshlets.size(); ++i)
		{
			Meshlet m = mesh.meshlets[i];
			m.vertexOffset    += uniqueVtxOffset;
			m.primitiveOffset += primIdxOffset;
			allMeshlets.push_back(m);
			allBounds.push_back(mesh.bounds[i]);
		}
		allUniqueVtx.insert(allUniqueVtx.end(), mesh.uniqueVertexIndices.begin(), mesh.uniqueVertexIndices.end());
		allPrimIdx.insert(allPrimIdx.end(), mesh.primitiveIndices.begin(), mesh.primitiveIndices.end());

		meshletOffset  += static_cast<uint32_t>(mesh.meshlets.size());
		uniqueVtxOffset += static_cast<uint32_t>(mesh.uniqueVertexIndices.size());
		primIdxOffset   += static_cast<uint32_t>(mesh.primitiveIndices.size());
	}

	if (allMeshlets.empty()) return false;

	// Upload buffers (kept alive by ComPtr in upload locals — caller must execute cmd before they go out of scope)
	ComPtr<ID3D12Resource> uploadMeshlet, uploadBounds, uploadUniqueVtx, uploadPrimIdx, uploadVerts;

	out.meshletBuffer = CreateDefaultBuffer(device, cmd,
		allMeshlets.data(), allMeshlets.size() * sizeof(Meshlet), uploadMeshlet);
	out.boundsBuffer = CreateDefaultBuffer(device, cmd,
		allBounds.data(), allBounds.size() * sizeof(MeshletBounds), uploadBounds);
	out.uniqueVertexIdxBuffer = CreateDefaultBuffer(device, cmd,
		allUniqueVtx.data(), allUniqueVtx.size() * sizeof(uint32_t), uploadUniqueVtx);
	out.primitiveIdxBuffer = CreateDefaultBuffer(device, cmd,
		allPrimIdx.data(), allPrimIdx.size() * sizeof(uint32_t), uploadPrimIdx);
	out.vertexBuffer = CreateDefaultBuffer(device, cmd,
		allVertices, totalVertexCount * sizeof(Vertex), uploadVerts);

	// Register in bindless heap
	out.meshletBufferSrvIdx = RegisterStructuredBufferSRV(heap, out.meshletBuffer.Get(),
		static_cast<uint32_t>(allMeshlets.size()), sizeof(Meshlet));
	out.boundsBufferSrvIdx = RegisterStructuredBufferSRV(heap, out.boundsBuffer.Get(),
		static_cast<uint32_t>(allBounds.size()), sizeof(MeshletBounds));
	out.uniqueVertexIdxSrvIdx = RegisterRawBufferSRV(heap, out.uniqueVertexIdxBuffer.Get(),
		static_cast<uint32_t>(allUniqueVtx.size() * sizeof(uint32_t)));
	out.primitiveIdxSrvIdx = RegisterRawBufferSRV(heap, out.primitiveIdxBuffer.Get(),
		static_cast<uint32_t>(allPrimIdx.size() * sizeof(uint32_t)));
	out.vertexBufferSrvIdx = RegisterStructuredBufferSRV(heap, out.vertexBuffer.Get(),
		totalVertexCount, sizeof(Vertex));

	out.totalMeshlets = static_cast<uint32_t>(allMeshlets.size());
	out.totalVertices = totalVertexCount;

	printf("MeshletBuilder::UploadToGpu: %u meshlets, %u verts, %zu uniqueIdx, %zu primIdx\n",
		out.totalMeshlets, totalVertexCount, allUniqueVtx.size(), allPrimIdx.size());
	return true;
}
