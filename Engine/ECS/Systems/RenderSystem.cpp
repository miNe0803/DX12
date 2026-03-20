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
#include <DirectXCollision.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
	RenderSystem::GpuDrawStats s_lastGpuStats;
	std::vector<RenderSystem::RenderQueueEntry> s_lastRenderQueue;
	bool s_frustumCullPbrEnabled = true;

	/// SceneConstants の View/Proj（シェーダと同一）から視錐台を作り、ローカル AABB をワールド AABB に変換して交差判定。
	/// （SDK によっては BoundingBox→OBB の Transform が無いため、ワールド AABB は回転時やや保守的＝誤カリングしにくい）
	bool IsWorldAabbInFrustum(
		const DirectX::XMMATRIX& world,
		const ModelBounds& local,
		const DirectX::XMMATRIX& view,
		const DirectX::XMMATRIX& proj)
	{
		using namespace DirectX;
		if (!IsValidModelBounds(local))
			return true;
		const XMMATRIX vp = XMMatrixMultiply(view, proj);
		BoundingFrustum frustum;
		BoundingFrustum::CreateFromMatrix(frustum, vp);
		const XMFLOAT3 c{
			0.5f * (local.Min.x + local.Max.x),
			0.5f * (local.Min.y + local.Max.y),
			0.5f * (local.Min.z + local.Max.z) };
		const XMFLOAT3 e{
			0.5f * (local.Max.x - local.Min.x),
			0.5f * (local.Max.y - local.Min.y),
			0.5f * (local.Max.z - local.Min.z) };
		BoundingBox localBox(c, e);
		BoundingBox worldAabb;
		localBox.Transform(worldAabb, world);
		return frustum.Intersects(worldAabb);
	}

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

	struct PbrDrawBatch
	{
		D3D12_GPU_VIRTUAL_ADDRESS sceneConstantsGpu = 0;
		D3D12_GPU_VIRTUAL_ADDRESS pbrPropertyGpu = 0;
		D3D12_GPU_DESCRIPTOR_HANDLE materialGpu{};
		D3D12_GPU_DESCRIPTOR_HANDLE envCubemapGpu{};
		D3D12_GPU_VIRTUAL_ADDRESS instanceRingSrvGpu = 0;
		VertexBuffer* vb = nullptr;
		IndexBuffer* ib = nullptr;
		UINT indexCount = 0;
		UINT instanceCount = 0;
	};

	static void RecordPbrBatchesOnCmd(
		ID3D12GraphicsCommandList* cmd,
		ID3D12DescriptorHeap* heap,
		const std::vector<PbrDrawBatch>& batches,
		size_t batchBegin,
		size_t batchEnd,
		RootSignature* rootSignature,
		PipelineState* pipelineState,
		D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu,
		D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu,
		uint32_t& outDrawCalls,
		uint32_t& outInstances)
	{
		if (!cmd || batchBegin >= batchEnd)
			return;
		ID3D12DescriptorHeap* heaps[] = { heap };
		cmd->SetDescriptorHeaps(1, heaps);
		// 別 CL では OM バインドは継承されない。PSO が DSV 形式を要求するため必須。
		if (rtvCpu.ptr != 0 && dsvCpu.ptr != 0)
			cmd->OMSetRenderTargets(1, &rtvCpu, FALSE, &dsvCpu);
		ID3D12PipelineState* const pso = pipelineState ? pipelineState->Get() : nullptr;
		ID3D12RootSignature* const rs = rootSignature ? rootSignature->Get() : nullptr;
		if (!pso || !rs)
			return;
		for (size_t i = batchBegin; i < batchEnd; ++i)
		{
			const PbrDrawBatch& b = batches[i];
			if (!b.vb || !b.ib || b.instanceCount == 0)
				continue;
			cmd->SetPipelineState(pso);
			cmd->SetGraphicsRootSignature(rs);
			cmd->SetGraphicsRootConstantBufferView(0, b.sceneConstantsGpu);
			cmd->SetGraphicsRootConstantBufferView(1, b.pbrPropertyGpu);
			cmd->SetGraphicsRootDescriptorTable(3, b.materialGpu);
			if (b.envCubemapGpu.ptr != 0)
				cmd->SetGraphicsRootDescriptorTable(4, b.envCubemapGpu);
			cmd->SetGraphicsRootShaderResourceView(2, b.instanceRingSrvGpu);
			D3D12_VERTEX_BUFFER_VIEW vbView = b.vb->View();
			D3D12_INDEX_BUFFER_VIEW ibView = b.ib->View();
			cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmd->IASetVertexBuffers(0, 1, &vbView);
			cmd->IASetIndexBuffer(&ibView);
			cmd->DrawIndexedInstanced(b.indexCount, b.instanceCount, 0, 0, 0);
			++outDrawCalls;
			outInstances += b.instanceCount;
		}
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
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU,
	ID3D12GraphicsCommandList* cmdListPbrRecord0,
	ID3D12GraphicsCommandList* cmdListPbrRecord1,
	ID3D12DescriptorHeap* sharedSrvHeapForPbr,
	D3D12_CPU_DESCRIPTOR_HANDLE mainPassRtvCpu,
	D3D12_CPU_DESCRIPTOR_HANDLE mainPassDsvCpu)
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

	const bool parallelPbrRecord = pbrResourcesOk && cmdListPbrRecord0 && cmdListPbrRecord1 && sharedSrvHeapForPbr
		&& mainPassRtvCpu.ptr != 0 && mainPassDsvCpu.ptr != 0;

	std::vector<DrawSortItem> queue;
	queue.reserve(256);

	const SceneConstants* sceneC = sceneConstantsCB ? sceneConstantsCB->GetPtr<SceneConstants>() : nullptr;
	const DirectX::XMMATRIX viewM = sceneC ? sceneC->View : DirectX::XMMatrixIdentity();
	const DirectX::XMMATRIX projM = sceneC ? sceneC->Proj : DirectX::XMMatrixIdentity();
	uint32_t statPbrFrustumCulled = 0;

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

		const auto& transform = view.get<TransformComponent>(entity);
		if (!isTerrain && s_frustumCullPbrEnabled && sceneC)
		{
			bool isPlayerFamily = isPlayer;
			if (!isPlayerFamily && registry.all_of<ModelGroupChildComponent>(entity))
			{
				const auto& ch = registry.get<ModelGroupChildComponent>(entity);
				if (registry.valid(ch.parent) && registry.all_of<PlayerComponent>(ch.parent))
					isPlayerFamily = true;
			}

			// モデルグループの子: パーツごとの AABB はノード未焼き・スキンで枝先を表さないため、
			// 親の「全メッシュ結合バウンド」× 親 World で判定する（桜の花など）。
			const ModelBounds* cullBounds = nullptr;
			DirectX::XMMATRIX cullWorld = transform.WorldMatrix;
			if (const auto* link = registry.try_get<ModelGroupChildComponent>(entity))
			{
				if (registry.valid(link->parent) && registry.all_of<ModelGroupRootComponent>(link->parent))
				{
					const auto& rootComp = registry.get<ModelGroupRootComponent>(link->parent);
					if (rootComp.hasCombinedModelBounds)
					{
						cullBounds = &rootComp.combinedModelBounds;
						cullWorld = registry.get<TransformComponent>(link->parent).WorldMatrix;
					}
				}
			}
			if (!cullBounds && mesh.HasLocalBounds)
				cullBounds = &mesh.LocalBounds;

			if (cullBounds && !mesh.SkipCpuFrustumCull && !isPlayerFamily
				&& !IsWorldAabbInFrustum(cullWorld, *cullBounds, sceneC->View, sceneC->Proj))
			{
				++statPbrFrustumCulled;
				continue;
			}
		}

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

	std::uint8_t* const cbBase = perDrawTransformCB ? reinterpret_cast<std::uint8_t*>(perDrawTransformCB->GetPtr()) : nullptr;
	const D3D12_GPU_VIRTUAL_ADDRESS cbGpuBase = perDrawTransformCB ? perDrawTransformCB->GetAddress() : 0;

	const UINT sliceBase = static_cast<UINT>(instanceRingFrameIndex * kMaxPbrInstancesPerFrame);
	const UINT sliceEnd = sliceBase + static_cast<UINT>(kMaxPbrInstancesPerFrame);

	UINT writeCursor = sliceBase;

	UINT pbrBatchCount = 0;
	UINT64 pbrBatchMaterialKey = 0;
	VertexBuffer* pbrBatchVB = nullptr;
	IndexBuffer* pbrBatchIB = nullptr;
	UINT pbrBatchIndexCount = 0;
	UINT pbrBatchRingStart = sliceBase;
	D3D12_GPU_DESCRIPTOR_HANDLE pbrBatchMaterialGpu{};

	std::vector<PbrDrawBatch> pbrBatches;
	if (parallelPbrRecord)
		pbrBatches.reserve(128);

	size_t terrainDrawSlot = 0;
	uint32_t statTotal = 0, statTerr = 0, statPbrBatches = 0, statPbrInstances = 0, statPlayerSub = 0;

	auto flushPbrBatchLegacy = [&]()
	{
		if (pbrBatchCount == 0 || !pbrBatchVB || !pbrBatchIB)
			return;
		// Root SRV is already rebased to batch start; instanceID starts from 0 in VS.
		cmdList->DrawIndexedInstanced(pbrBatchIndexCount, pbrBatchCount, 0, 0, 0);
		++statTotal;
		++statPbrBatches;
		statPbrInstances += pbrBatchCount;
		pbrBatchCount = 0;
	};

	auto flushPbrBatchToVector = [&]()
	{
		if (!parallelPbrRecord || pbrBatchCount == 0 || !pbrBatchVB || !pbrBatchIB)
			return;
		PbrDrawBatch b{};
		b.sceneConstantsGpu = sceneConstantsCB->GetAddress();
		b.pbrPropertyGpu = pbrPropertyBuffer->GetAddress();
		b.materialGpu = pbrBatchMaterialGpu;
		b.envCubemapGpu = envCubemapHandleGPU;
		b.instanceRingSrvGpu = pbrInstanceBuffer->GetGPUVirtualAddress()
			+ static_cast<UINT64>(pbrBatchRingStart) * sizeof(InstanceData);
		b.vb = pbrBatchVB;
		b.ib = pbrBatchIB;
		b.indexCount = pbrBatchIndexCount;
		b.instanceCount = pbrBatchCount;
		pbrBatches.push_back(b);
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
			if (parallelPbrRecord)
				flushPbrBatchToVector();
			else
				flushPbrBatchLegacy();

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
			if (parallelPbrRecord)
			{
				flushPbrBatchToVector();
				pbrBatchRingStart = writeCursor;
				pbrBatchMaterialKey = it.materialSortKey;
				pbrBatchMaterialGpu = mesh.MaterialHandle->HandleGPU;
				pbrBatchVB = it.vb;
				pbrBatchIB = it.ib;
				pbrBatchIndexCount = it.indexCount;
			}
			else
			{
				flushPbrBatchLegacy();

				cmdList->SetPipelineState(pipelineState->Get());
				cmdList->SetGraphicsRootSignature(rootSignature->Get());
				cmdList->SetGraphicsRootConstantBufferView(0, sceneConstantsCB->GetAddress());
				cmdList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer->GetAddress());
				cmdList->SetGraphicsRootDescriptorTable(3, mesh.MaterialHandle->HandleGPU);
				if (envCubemapHandleGPU.ptr != 0)
					cmdList->SetGraphicsRootDescriptorTable(4, envCubemapHandleGPU);

				const UINT64 batchBaseOffsetBytes = static_cast<UINT64>(writeCursor) * sizeof(InstanceData);
				cmdList->SetGraphicsRootShaderResourceView(2, pbrInstanceBuffer->GetGPUVirtualAddress() + batchBaseOffsetBytes);

				D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
				D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
				cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				cmdList->IASetVertexBuffers(0, 1, &vbView);
				cmdList->IASetIndexBuffer(&ibView);

				pbrBatchMaterialKey = it.materialSortKey;
				pbrBatchVB = it.vb;
				pbrBatchIB = it.ib;
				pbrBatchIndexCount = it.indexCount;
			}
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

	if (parallelPbrRecord)
	{
		flushPbrBatchToVector();
		const size_t n = pbrBatches.size();
		const size_t mid = (n + 1) / 2;
		uint32_t d0 = 0, inst0 = 0, d1 = 0, inst1 = 0;
		std::thread worker0([&]()
		{
			RecordPbrBatchesOnCmd(cmdListPbrRecord0, sharedSrvHeapForPbr, pbrBatches, 0, mid,
				rootSignature, pipelineState, mainPassRtvCpu, mainPassDsvCpu, d0, inst0);
		});
		std::thread worker1([&]()
		{
			RecordPbrBatchesOnCmd(cmdListPbrRecord1, sharedSrvHeapForPbr, pbrBatches, mid, n,
				rootSignature, pipelineState, mainPassRtvCpu, mainPassDsvCpu, d1, inst1);
		});
		worker0.join();
		worker1.join();
		statPbrBatches = d0 + d1;
		statPbrInstances = inst0 + inst1;
		statTotal += statPbrBatches;
	}
	else
	{
		flushPbrBatchLegacy();
	}

	s_lastGpuStats.drawIndexedTotal = statTotal;
	s_lastGpuStats.terrainDraws = statTerr;
	s_lastGpuStats.pbrBatchDrawCalls = statPbrBatches;
	s_lastGpuStats.pbrInstancesDrawn = statPbrInstances;
	s_lastGpuStats.playerModelSubmeshDraws = statPlayerSub;
	s_lastGpuStats.pbrFrustumCulledEntities = statPbrFrustumCulled;
}

bool RenderSystem::GetFrustumCullPbrEnabled()
{
	return s_frustumCullPbrEnabled;
}

void RenderSystem::SetFrustumCullPbrEnabled(bool enabled)
{
	s_frustumCullPbrEnabled = enabled;
}
