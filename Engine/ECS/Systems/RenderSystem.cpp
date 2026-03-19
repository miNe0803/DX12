#include "RenderSystem.h"
#include "../Components.h"
#include "SharedStruct.h"
#include "ConstantBuffer.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "DescriptorHeap.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "DebugLog.h"
#include <DirectXMath.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
	RenderSystem::GpuDrawStats s_lastGpuStats;
	std::vector<RenderSystem::RenderQueueEntry> s_lastRenderQueue;

	struct DrawSortItem
	{
		entt::entity entity;
		bool isTerrain;
		ID3D12PipelineState* pso;
		UINT64 materialSortKey;
		VertexBuffer* vb;
		IndexBuffer* ib;
		UINT indexCount;
	};

	bool DrawSortLess(const DrawSortItem& a, const DrawSortItem& b)
	{
		const uintptr_t pa = reinterpret_cast<uintptr_t>(a.pso);
		const uintptr_t pb = reinterpret_cast<uintptr_t>(b.pso);
		if (pa != pb)
			return pa < pb;
		if (a.materialSortKey != b.materialSortKey)
			return a.materialSortKey < b.materialSortKey;
		const uintptr_t vba = reinterpret_cast<uintptr_t>(a.vb);
		const uintptr_t vbb = reinterpret_cast<uintptr_t>(b.vb);
		if (vba != vbb)
			return vba < vbb;
		const uintptr_t iba = reinterpret_cast<uintptr_t>(a.ib);
		const uintptr_t ibb = reinterpret_cast<uintptr_t>(b.ib);
		if (iba != ibb)
			return iba < ibb;
		return a.indexCount < b.indexCount;
	}
}

const RenderSystem::RenderQueueEntry* RenderSystem::GetLastRenderQueue()
{
	return s_lastRenderQueue.empty() ? nullptr : s_lastRenderQueue.data();
}

std::size_t RenderSystem::GetLastRenderQueueSize()
{
	return s_lastRenderQueue.size();
}

RenderSystem::GpuDrawStats RenderSystem::GetLastGpuDrawStats()
{
	return s_lastGpuStats;
}

void RenderSystem::DrawMain(
	entt::registry& registry,
	ID3D12GraphicsCommandList* cmdList,
	ConstantBuffer* perDrawTransformCB,
	ConstantBuffer* sceneConstantsCB,
	ConstantBuffer* pbrPropertyBuffer,
	ID3D12Resource* pbrInstanceBuffer,
	InstanceData* pbrInstanceMapped,
	UINT instanceRingFrameIndex,
	RootSignature* rootSignature,
	PipelineState* pipelineState,
	DescriptorHeap* descriptorHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE envCubemapHandleGPU,
	RootSignature* terrainRootSignature,
	PipelineState* terrainPipelineState,
	ConstantBuffer* terrainCB,
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU)
{
	(void)descriptorHeap;
	auto view = registry.view<TransformComponent, MeshRendererComponent, LODComponent>();
	const bool useTerrain = terrainRootSignature && terrainRootSignature->IsValid()
		&& terrainPipelineState && terrainPipelineState->IsValid()
		&& terrainCB && terrainCB->GetAddress() && terrainMaskHandleGPU.ptr != 0;

	ID3D12PipelineState* const psoPbr = pipelineState ? pipelineState->Get() : nullptr;
	ID3D12PipelineState* const psoTerrain = terrainPipelineState ? terrainPipelineState->Get() : nullptr;
	const UINT64 terrainMaterialKey = terrainMaskHandleGPU.ptr;

	const bool pbrResourcesOk = sceneConstantsCB && sceneConstantsCB->GetAddress()
		&& pbrPropertyBuffer && pbrPropertyBuffer->GetAddress()
		&& pbrInstanceBuffer && pbrInstanceMapped && psoPbr;

	std::vector<DrawSortItem> queue;
	queue.reserve(256);

	for (auto entity : view)
	{
		const auto& lod = view.get<LODComponent>(entity);
		if (lod.CurrentLODLevel == 3)
		{
			if (registry.all_of<PlayerComponent>(entity))
				DebugLog("[Player][Draw] SKIP entity=%u (LOD culled level=3)\n", static_cast<unsigned>(entity));
			continue;
		}

		const auto& mesh = view.get<MeshRendererComponent>(entity);
		const bool isPlayer = registry.all_of<PlayerComponent>(entity);

		if (!mesh.pVB || !mesh.pIB)
		{
			if (isPlayer)
			{
				DebugLog("[Player][Draw] SKIP entity=%u (no VB/IB) vb=%p ib=%p\n",
					static_cast<unsigned>(entity),
					static_cast<void*>(mesh.pVB),
					static_cast<void*>(mesh.pIB));
			}
			continue;
		}

		const bool isTerrain = useTerrain && registry.all_of<TerrainComponent>(entity);
		if (isTerrain)
		{
			if (!perDrawTransformCB || !perDrawTransformCB->GetPtr())
			{
				if (isPlayer)
					DebugLog("[Player][Draw] SKIP terrain entity=%u (no per-draw CB)\n", static_cast<unsigned>(entity));
				continue;
			}
		}
		else
		{
			if (!pbrResourcesOk || !mesh.MaterialHandle)
			{
				if (isPlayer)
				{
					DebugLog("[Player][Draw] SKIP entity=%u (PBR resources or material) material=%p\n",
						static_cast<unsigned>(entity),
						static_cast<void*>(mesh.MaterialHandle));
				}
				continue;
			}
		}

		if (isTerrain && !psoTerrain)
			continue;

		DrawSortItem it{};
		it.entity = entity;
		it.isTerrain = isTerrain;
		it.pso = isTerrain ? psoTerrain : psoPbr;
		it.materialSortKey = isTerrain ? terrainMaterialKey : mesh.MaterialHandle->HandleGPU.ptr;
		it.vb = mesh.pVB;
		it.ib = mesh.pIB;
		it.indexCount = mesh.IndexCount;
		queue.push_back(it);
	}

	std::sort(queue.begin(), queue.end(), DrawSortLess);

	s_lastRenderQueue.resize(queue.size());
	for (size_t i = 0; i < queue.size(); ++i)
	{
		s_lastRenderQueue[i].Entity = queue[i].entity;
		s_lastRenderQueue[i].IsTerrain = queue[i].isTerrain;
	}

	const SceneConstants* sceneC = sceneConstantsCB ? sceneConstantsCB->GetPtr<SceneConstants>() : nullptr;
	const DirectX::XMMATRIX viewM = sceneC ? sceneC->View : DirectX::XMMatrixIdentity();
	const DirectX::XMMATRIX projM = sceneC ? sceneC->Proj : DirectX::XMMatrixIdentity();

	std::uint8_t* const cbBase = perDrawTransformCB ? reinterpret_cast<std::uint8_t*>(perDrawTransformCB->GetPtr()) : nullptr;
	const D3D12_GPU_VIRTUAL_ADDRESS cbGpuBase = perDrawTransformCB ? perDrawTransformCB->GetAddress() : 0;

	const UINT sliceBase = static_cast<UINT>(instanceRingFrameIndex * kMaxPbrInstancesPerFrame);
	const UINT sliceEnd = sliceBase + static_cast<UINT>(kMaxPbrInstancesPerFrame);

	UINT writeCursor = sliceBase;

	UINT pbrBatchCount = 0;
	UINT pbrBatchStartInstanceLocation = 0;
	UINT64 pbrBatchMaterialKey = 0;
	VertexBuffer* pbrBatchVB = nullptr;
	IndexBuffer* pbrBatchIB = nullptr;
	UINT pbrBatchIndexCount = 0;

	size_t terrainDrawSlot = 0;
	uint32_t statTotal = 0, statTerr = 0, statPbrBatches = 0, statPbrInstances = 0, statPlayerSub = 0;

	auto flushPbrBatch = [&]()
	{
		if (pbrBatchCount == 0 || !pbrBatchVB || !pbrBatchIB)
			return;
		cmdList->DrawIndexedInstanced(pbrBatchIndexCount, pbrBatchCount, 0, 0, pbrBatchStartInstanceLocation);
		++statTotal;
		++statPbrBatches;
		statPbrInstances += pbrBatchCount;
		pbrBatchCount = 0;
	};

	for (const DrawSortItem& it : queue)
	{
		const auto& transform = registry.get<TransformComponent>(it.entity);
		const auto& mesh = registry.get<MeshRendererComponent>(it.entity);
		const bool isPlayer = registry.all_of<PlayerComponent>(it.entity);
		const auto& lod = registry.get<LODComponent>(it.entity);

		if (it.isTerrain)
		{
			flushPbrBatch();

			if (terrainDrawSlot >= kPerDrawTransformSlotCount)
			{
				DebugLog("[Render] Transform CB slots exhausted (max=%zu)\n", kPerDrawTransformSlotCount);
				continue;
			}
			Transform* pTransform = reinterpret_cast<Transform*>(cbBase + terrainDrawSlot * sizeof(Transform));
			pTransform->View = viewM;
			pTransform->Proj = projM;
			pTransform->World = transform.WorldMatrix;
			const D3D12_GPU_VIRTUAL_ADDRESS cbThisDraw = cbGpuBase + terrainDrawSlot * sizeof(Transform);
			++terrainDrawSlot;

			cmdList->SetPipelineState(terrainPipelineState->Get());
			cmdList->SetGraphicsRootSignature(terrainRootSignature->Get());
			cmdList->SetGraphicsRootConstantBufferView(0, cbThisDraw);
			cmdList->SetGraphicsRootConstantBufferView(1, terrainCB->GetAddress());
			cmdList->SetGraphicsRootDescriptorTable(2, terrainMaskHandleGPU);
			if (envCubemapHandleGPU.ptr != 0)
				cmdList->SetGraphicsRootDescriptorTable(3, envCubemapHandleGPU);

			D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
			D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmdList->IASetVertexBuffers(0, 1, &vbView);
			cmdList->IASetIndexBuffer(&ibView);
			cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
			++statTotal;
			++statTerr;

			if (isPlayer)
			{
				DebugLog("[Player][Draw] OK entity=%u DrawIndexed indices=%u lod=%d pos=(%.3f,%.3f,%.3f)\n",
					static_cast<unsigned>(it.entity),
					mesh.IndexCount,
					lod.CurrentLODLevel,
					transform.Position.x, transform.Position.y, transform.Position.z);
			}
			continue;
		}

		// PBR instanced
		const bool sameBatch = pbrBatchCount > 0
			&& pbrBatchMaterialKey == it.materialSortKey
			&& pbrBatchVB == it.vb && pbrBatchIB == it.ib && pbrBatchIndexCount == it.indexCount;

		if (!sameBatch)
		{
			flushPbrBatch();

			cmdList->SetPipelineState(pipelineState->Get());
			cmdList->SetGraphicsRootSignature(rootSignature->Get());
			cmdList->SetGraphicsRootConstantBufferView(0, sceneConstantsCB->GetAddress());
			cmdList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer->GetAddress());
			cmdList->SetGraphicsRootDescriptorTable(3, mesh.MaterialHandle->HandleGPU);
			if (envCubemapHandleGPU.ptr != 0)
				cmdList->SetGraphicsRootDescriptorTable(4, envCubemapHandleGPU);

			// Root signature parameter [2] is the instance SRV (PBR path).
			// Must be bound after switching away from terrain root signature.
			cmdList->SetGraphicsRootShaderResourceView(2, pbrInstanceBuffer->GetGPUVirtualAddress());

			D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
			D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmdList->IASetVertexBuffers(0, 1, &vbView);
			cmdList->IASetIndexBuffer(&ibView);

			pbrBatchStartInstanceLocation = writeCursor;
			pbrBatchMaterialKey = it.materialSortKey;
			pbrBatchVB = it.vb;
			pbrBatchIB = it.ib;
			pbrBatchIndexCount = it.indexCount;
		}

		if (writeCursor >= sliceEnd)
		{
			DebugLog("[Render] PBR instance ring slice full (max=%zu per frame)\n", kMaxPbrInstancesPerFrame);
			continue;
		}

		DirectX::XMStoreFloat4x4(&pbrInstanceMapped[writeCursor].World, transform.WorldMatrix);
		++writeCursor;
		++pbrBatchCount;

		if (const auto* link = registry.try_get<ModelGroupChildComponent>(it.entity))
		{
			if (registry.valid(link->parent) && registry.all_of<PlayerComponent>(link->parent))
				++statPlayerSub;
		}

		if (isPlayer)
		{
			DebugLog("[Player][Draw] OK entity=%u instanced indices=%u lod=%d pos=(%.3f,%.3f,%.3f)\n",
				static_cast<unsigned>(it.entity),
				mesh.IndexCount,
				lod.CurrentLODLevel,
				transform.Position.x, transform.Position.y, transform.Position.z);
		}
	}

	flushPbrBatch();

	s_lastGpuStats.drawIndexedTotal = statTotal;
	s_lastGpuStats.terrainDraws = statTerr;
	s_lastGpuStats.pbrBatchDrawCalls = statPbrBatches;
	s_lastGpuStats.pbrInstancesDrawn = statPbrInstances;
	s_lastGpuStats.playerModelSubmeshDraws = statPlayerSub;
}
