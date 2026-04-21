#include "RenderSystem.h"
#include "../Components.h"
#include "Graphics/TerrainGpuCullSystem.h"
#include "SharedStruct.h"
#include "ConstantBuffer.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "DescriptorHeap.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "Graphics/TreeGpuCullSystem.h"
#include "DebugLog.h"
#include "Core/GpuDebugLabels.h"
#include "Engine/Profiling/Profiler.h"
#include <DirectXMath.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <thread>
#include <vector>

namespace
{
	RenderSystem::GpuDrawStats s_lastGpuStats;
	std::vector<RenderSystem::RenderQueueEntry> s_lastRenderQueue;
	bool s_frustumCullPbrEnabled = true;
	// Forest uses mask + GPU ExecuteIndirect (Post CL). ECS TreeInstanceTag meshes are not drawn in this PBR pass (avoids double draw).
	constexpr bool kSkipEcsTreeMeshesInMainPbrPass = true;
	/// DrawMain 終了時点のインスタンスリング書き込み終端（exclusive）。NPR が続きから使用。
	uint32_t s_instanceRingWriteEndExclusive = 0;

	/// SceneConstants の View/Proj（シェーダと同一: mul(mul(worldPos, View), Proj)）に合わせ、ローカル AABB の 8 頂点を
	/// 同次クリップ空間へ変換して視錐台の外側かどうかを判定する。
	/// BoundingFrustum::CreateFromMatrix は投影行列単体向けで、結合 VP では Origin=0 の表現とワールド AABB が噛み合わず
	/// 角度依存の誤判定になるため使わない。
	/// TransformComponent::WorldMatrix は GPU 用に XMMatrixTranspose 済み → 行メジャー World は XMMatrixTranspose(world)。
	bool IsWorldAabbInFrustum(
		const DirectX::XMMATRIX& worldGpuT,
		const ModelBounds& local,
		const DirectX::XMMATRIX& viewGpuT,
		const DirectX::XMMATRIX& projGpuT)
	{
		using namespace DirectX;
		if (!IsValidModelBounds(local))
			return true;

		const XMMATRIX worldRow = XMMatrixTranspose(worldGpuT);
		const XMMATRIX viewRow = XMMatrixTranspose(viewGpuT);
		const XMMATRIX projRow = XMMatrixTranspose(projGpuT);
		const XMMATRIX vp = XMMatrixMultiply(viewRow, projRow);

		const XMVECTOR bmin = XMLoadFloat3(&local.Min);
		const XMVECTOR bmax = XMLoadFloat3(&local.Max);
		const float mx = XMVectorGetX(bmax), my = XMVectorGetY(bmax), mz = XMVectorGetZ(bmax);
		const float nx = XMVectorGetX(bmin), ny = XMVectorGetY(bmin), nz = XMVectorGetZ(bmin);

		XMVECTOR clipCorners[8];
		for (int i = 0; i < 8; ++i)
		{
			const float x = (i & 1) ? mx : nx;
			const float y = (i & 2) ? my : ny;
			const float z = (i & 4) ? mz : nz;
			const XMVECTOR localPos = XMVectorSet(x, y, z, 1.f);
			const XMVECTOR wpos = XMVector4Transform(localPos, worldRow);
			clipCorners[i] = XMVector4Transform(wpos, vp);
		}

		// w<=0 の頂点があると同次平面の「全点が外」の判定が不安定になるため保守的に可視とする。
		for (int i = 0; i < 8; ++i)
		{
			if (XMVectorGetW(clipCorners[i]) <= 0.f)
				return true;
		}

		auto allOutsideLeft = [&]() -> bool {
			for (int i = 0; i < 8; ++i)
			{
				const XMVECTOR c = clipCorners[i];
				if (!(XMVectorGetX(c) < -XMVectorGetW(c)))
					return false;
			}
			return true;
		};
		auto allOutsideRight = [&]() -> bool {
			for (int i = 0; i < 8; ++i)
			{
				const XMVECTOR c = clipCorners[i];
				if (!(XMVectorGetX(c) > XMVectorGetW(c)))
					return false;
			}
			return true;
		};
		auto allOutsideBottom = [&]() -> bool {
			for (int i = 0; i < 8; ++i)
			{
				const XMVECTOR c = clipCorners[i];
				if (!(XMVectorGetY(c) < -XMVectorGetW(c)))
					return false;
			}
			return true;
		};
		auto allOutsideTop = [&]() -> bool {
			for (int i = 0; i < 8; ++i)
			{
				const XMVECTOR c = clipCorners[i];
				if (!(XMVectorGetY(c) > XMVectorGetW(c)))
					return false;
			}
			return true;
		};
		auto allOutsideNear = [&]() -> bool {
			for (int i = 0; i < 8; ++i)
			{
				if (!(XMVectorGetZ(clipCorners[i]) < 0.f))
					return false;
			}
			return true;
		};
		auto allOutsideFar = [&]() -> bool {
			for (int i = 0; i < 8; ++i)
			{
				const XMVECTOR c = clipCorners[i];
				if (!(XMVectorGetZ(c) > XMVectorGetW(c)))
					return false;
			}
			return true;
		};

		if (allOutsideLeft() || allOutsideRight() || allOutsideBottom() || allOutsideTop() || allOutsideNear() || allOutsideFar())
			return false;

		return true;
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

	/// RenderDoc で検索しやすいよう「Player」PIX 用（親に PlayerComponent のモデルグループ子など）
	bool EntityIsPlayerFamilyMesh(entt::registry& registry, entt::entity entity)
	{
		if (registry.all_of<PlayerComponent>(entity))
			return true;
		if (const auto* link = registry.try_get<ModelGroupChildComponent>(entity))
			return registry.valid(link->parent) && registry.all_of<PlayerComponent>(link->parent);
		return false;
	}

	/// 木ルートに TreeInstanceTag、パートは ModelGroupChild（親にタグ）
	bool EntityIsTreeMeshDraw(entt::registry& registry, entt::entity entity)
	{
		if (registry.all_of<TreeInstanceTag>(entity))
			return true;
		if (const auto* link = registry.try_get<ModelGroupChildComponent>(entity))
			return registry.valid(link->parent) && registry.all_of<TreeInstanceTag>(link->parent);
		return false;
	}

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
		if (a.indexCount != b.indexCount)
			return a.indexCount < b.indexCount;
		// 同キー完全同一だと std::sort の順序が不定 → 深度/透明の見え方がフレームで変わる。entity で安定化。
		return static_cast<std::underlying_type_t<entt::entity>>(a.entity)
			< static_cast<std::underlying_type_t<entt::entity>>(b.entity);
	}

	/// TreeInstanceTag 親の子どうしだけ siblingDrawOrder で並べ替え（stable_sort 用）
	bool TreeSiblingDrawOrderLess(entt::registry& reg, const DrawSortItem& a, const DrawSortItem& b)
	{
		const auto* ca = reg.try_get<ModelGroupChildComponent>(a.entity);
		const auto* cb = reg.try_get<ModelGroupChildComponent>(b.entity);
		if (!ca || !cb || !reg.valid(ca->parent) || ca->parent != cb->parent)
			return false;
		if (!reg.all_of<TreeInstanceTag>(ca->parent))
			return false;
		return ca->siblingDrawOrder < cb->siblingDrawOrder;
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
		bool hasPlayerInstance = false;
		ID3D12PipelineState* pso = nullptr;
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
		GPU_CMD_BEGIN_EVENT(cmd, 100, 200, 255, L"PBR: DrawIndexedInstanced batches (parallel CL)");
		for (size_t i = batchBegin; i < batchEnd; ++i)
		{
			const PbrDrawBatch& b = batches[i];
			if (!b.vb || !b.ib || b.instanceCount == 0)
				continue;
			if (b.hasPlayerInstance)
				GPU_CMD_BEGIN_EVENT(cmd, 96, 220, 112, L"Player: PBR batch (DrawIndexedInstanced)");
			ID3D12PipelineState* batchPso = b.pso ? b.pso : pso;
			cmd->SetPipelineState(batchPso);
			cmd->SetGraphicsRootSignature(rs);
			cmd->SetGraphicsRootConstantBufferView(0, b.sceneConstantsGpu);
			cmd->SetGraphicsRootConstantBufferView(1, b.pbrPropertyGpu);
			cmd->SetGraphicsRootDescriptorTable(3, b.materialGpu);
			if (b.envCubemapGpu.ptr != 0)
				cmd->SetGraphicsRootDescriptorTable(4, b.envCubemapGpu);
			cmd->SetGraphicsRootShaderResourceView(2, b.instanceRingSrvGpu);
			// TreeIndirectVS-only params: keep deterministic defaults for non-tree draws.
			cmd->SetGraphicsRootShaderResourceView(5, 0);
			const UINT treeDefaults[4] = { 0, 0, 0, 0 };
			cmd->SetGraphicsRoot32BitConstants(6, 4, treeDefaults, 0);
			D3D12_VERTEX_BUFFER_VIEW vbView = b.vb->View();
			D3D12_INDEX_BUFFER_VIEW ibView = b.ib->View();
			cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmd->IASetVertexBuffers(0, 1, &vbView);
			cmd->IASetIndexBuffer(&ibView);
			cmd->DrawIndexedInstanced(b.indexCount, b.instanceCount, 0, 0, 0);
			if (b.hasPlayerInstance)
				GPU_CMD_END_EVENT(cmd);
			++outDrawCalls;
			outInstances += b.instanceCount;
		}
		GPU_CMD_END_EVENT(cmd);
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
	PipelineState* terrainDepthPrepassPipelineState,
	PipelineState* terrainPipelineState,
	ConstantBuffer* terrainCB,
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU,
	ID3D12GraphicsCommandList* cmdListPbrRecord0,
	ID3D12GraphicsCommandList* cmdListPbrRecord1,
	ID3D12DescriptorHeap* sharedSrvHeapForPbr,
	D3D12_CPU_DESCRIPTOR_HANDLE mainPassRtvCpu,
	D3D12_CPU_DESCRIPTOR_HANDLE mainPassDsvCpu,
	bool nprOpaquePsoValid,
	bool nprTransparentPsoValid,
	TerrainGpuCullSystem* terrainGpuCull,
	VertexBuffer* terrainSharedVB,
	IndexBuffer* terrainSharedIB,
	PipelineState* treeInstancingLod1Pso,
	PipelineState* treeInstancingLod2Pso,
	D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSrvGpu,
	D3D12_GPU_VIRTUAL_ADDRESS shadowCBGpu)
{
	(void)descriptorHeap;
	auto view = registry.view<TransformComponent, MeshRendererComponent, LODComponent>();
	const bool useTerrain = terrainRootSignature && terrainRootSignature->IsValid()
		&& terrainPipelineState && terrainPipelineState->IsValid()
		&& terrainCB && terrainCB->GetAddress() && terrainMaskHandleGPU.ptr != 0;
	const bool useTerrainDepthPrepass = useTerrain
		&& terrainDepthPrepassPipelineState && terrainDepthPrepassPipelineState->IsValid();
	const bool gpuTerrainCull = terrainGpuCull && terrainGpuCull->IsValid() && useTerrain;

	ID3D12PipelineState* const psoPbr = pipelineState ? pipelineState->Get() : nullptr;
	ID3D12PipelineState* const psoTreeLod1 = (treeInstancingLod1Pso && treeInstancingLod1Pso->IsValid())
		? treeInstancingLod1Pso->Get() : psoPbr;
	ID3D12PipelineState* const psoTreeLod2 = (treeInstancingLod2Pso && treeInstancingLod2Pso->IsValid())
		? treeInstancingLod2Pso->Get() : psoPbr;
	ID3D12PipelineState* const psoTerrain = terrainPipelineState ? terrainPipelineState->Get() : nullptr;
	const UINT64 terrainMaterialKey = terrainMaskHandleGPU.ptr;

	const bool pbrResourcesOk = sceneConstantsCB && sceneConstantsCB->GetAddress()
		&& pbrPropertyBuffer && pbrPropertyBuffer->GetAddress()
		&& pbrInstanceBuffer && pbrInstanceMapped && psoPbr;

	const bool parallelPbrRecord = pbrResourcesOk && cmdListPbrRecord0 && cmdListPbrRecord1 && sharedSrvHeapForPbr
		&& mainPassRtvCpu.ptr != 0 && mainPassDsvCpu.ptr != 0;

	static std::vector<DrawSortItem> queue;
	queue.clear(); // static で再利用 — 毎フレームの heap alloc を回避

	const SceneConstants* sceneC = sceneConstantsCB ? sceneConstantsCB->GetPtr<SceneConstants>() : nullptr;
	const DirectX::XMMATRIX viewM = sceneC ? sceneC->View : DirectX::XMMatrixIdentity();
	const DirectX::XMMATRIX projM = sceneC ? sceneC->Proj : DirectX::XMMatrixIdentity();
	uint32_t statPbrFrustumCulled = 0;
	uint32_t statTreeTagCount = 0;
	uint32_t statTreeFrustumCulled = 0;
	for ([[maybe_unused]] entt::entity _te : registry.view<TreeInstanceTag>())
		++statTreeTagCount;

	if (gpuTerrainCull && sceneC)
		terrainGpuCull->DispatchFrustumCull(cmdList, sceneC);

	for (auto entity : view)
	{
		const auto& lod = view.get<LODComponent>(entity);
		if (lod.CurrentLODLevel == 3)
		{
			if (registry.all_of<PlayerComponent>(entity))
				//DebugLog("[Player][Draw] SKIP entity=%u (LOD culled level=3)\n", static_cast<unsigned>(entity));
			continue;
		}

		const auto& mesh = view.get<MeshRendererComponent>(entity);
		const bool isPlayer = registry.all_of<PlayerComponent>(entity);

		if (kSkipEcsTreeMeshesInMainPbrPass && EntityIsTreeMeshDraw(registry, entity))
			continue;

		if (!mesh.pVB || !mesh.pIB)
		{
			if (isPlayer)
			{
				//DebugLog("[Player][Draw] SKIP entity=%u (no VB/IB) vb=%p ib=%p\n",
				//	static_cast<unsigned>(entity),
				//	static_cast<void*>(mesh.pVB),
				//	static_cast<void*>(mesh.pIB));
			}
			continue;
		}

		const bool isTerrain = useTerrain && registry.all_of<TerrainMeshTag>(entity);
		if (isTerrain && gpuTerrainCull)
			continue;
		if (isTerrain)
		{
			if (!perDrawTransformCB || !perDrawTransformCB->GetPtr())
			{
				if (isPlayer)
					//DebugLog("[Player][Draw] SKIP terrain entity=%u (no per-draw CB)\n", static_cast<unsigned>(entity));
				continue;
			}
		}
		else
		{
			// NPR 親の子: 不透明は不透明 NPR PSO、透明は透明 NPR PSO が無いと DrawNprPasses で描けない。
			// 以前は「不透明 PSO だけ有効」でも全子を PBR から外しており、透明パーツが完全にドロップしていた。
			if (const auto* link = registry.try_get<ModelGroupChildComponent>(entity))
			{
				if (registry.valid(link->parent) && registry.all_of<NPRTag>(link->parent))
				{
					const bool nprWillDrawThisMesh =
						(!mesh.NprTransparent && nprOpaquePsoValid)
						|| (mesh.NprTransparent && nprTransparentPsoValid);
					if (nprWillDrawThisMesh)
						continue;
				}
			}
			if (!pbrResourcesOk || !mesh.MaterialHandle)
			{
				if (isPlayer)
				{
					//DebugLog("[Player][Draw] SKIP entity=%u (PBR resources or material) material=%p\n",
					//	static_cast<unsigned>(entity),
					//	static_cast<void*>(mesh.MaterialHandle));
				}
				continue;
			}
		}

		if (isTerrain && !psoTerrain)
			continue;

		const auto& transform = view.get<TransformComponent>(entity);
		if (isTerrain && sceneC && mesh.HasLocalBounds
			&& !IsWorldAabbInFrustum(transform.WorldMatrix, mesh.LocalBounds, sceneC->View, sceneC->Proj))
			continue;

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
				if (EntityIsTreeMeshDraw(registry, entity))
					++statTreeFrustumCulled;
				continue;
			}
		}

		DrawSortItem it{};
		it.entity = entity;
		it.isTerrain = isTerrain;
		ID3D12PipelineState* pbrPsoPick = psoPbr;
		if (!isTerrain && EntityIsTreeMeshDraw(registry, entity))
		{
			if (lod.CurrentLODLevel == 1)
				pbrPsoPick = psoTreeLod1;
			else if (lod.CurrentLODLevel == 2)
				pbrPsoPick = psoTreeLod2;
		}
		it.pso = isTerrain ? psoTerrain : pbrPsoPick;
		it.materialSortKey = isTerrain ? terrainMaterialKey : mesh.MaterialHandle->HandleGPU.ptr;
		it.vb = mesh.pVB;
		it.ib = mesh.pIB;
		it.indexCount = mesh.IndexCount;
		queue.push_back(it);
	}

	std::sort(queue.begin(), queue.end(), DrawSortLess);
	// 木の幹/枝/葉は同一ワールド行列でメッシュが重なる。マテリアルキー順だけだとパーツ順が不定になり Z でチラつく。
	std::stable_sort(queue.begin(), queue.end(),
		[&registry](const DrawSortItem& x, const DrawSortItem& y) {
			return TreeSiblingDrawOrderLess(registry, x, y);
		});

	uint32_t treeInDrawQueue = 0;
	for (const auto& qi : queue)
	{
		if (EntityIsTreeMeshDraw(registry, qi.entity))
			++treeInDrawQueue;
	}

	s_lastRenderQueue.resize(queue.size());
	for (size_t i = 0; i < queue.size(); ++i)
	{
		s_lastRenderQueue[i].Entity = queue[i].entity;
		s_lastRenderQueue[i].IsTerrain = queue[i].isTerrain;
	}

	std::uint8_t* const cbBase = perDrawTransformCB ? reinterpret_cast<std::uint8_t*>(perDrawTransformCB->GetPtr()) : nullptr;
	const D3D12_GPU_VIRTUAL_ADDRESS cbGpuBase = perDrawTransformCB ? perDrawTransformCB->GetAddress() : 0;

	size_t terrainDrawSlot = 0;
	uint32_t statTotal = 0, statTerr = 0, statPbrBatches = 0, statPbrInstances = 0, statPlayerSub = 0;

	const bool gpuTerrainDraw = gpuTerrainCull && terrainSharedVB && terrainSharedIB
		&& terrainRootSignature && terrainRootSignature->IsValid()
		&& terrainPipelineState && terrainPipelineState->IsValid()
		&& terrainCB && terrainMaskHandleGPU.ptr != 0;
	if (gpuTerrainDraw)
	{
		if (perDrawTransformCB && perDrawTransformCB->GetPtr())
		{
			DirectX::XMMATRIX terrainWorld = DirectX::XMMatrixIdentity();
			for (auto e : registry.view<TerrainMeshTag, TransformComponent>())
			{
				terrainWorld = registry.get<TransformComponent>(e).WorldMatrix;
				break;
			}
			Transform* pTransform = reinterpret_cast<Transform*>(cbBase + 0 * sizeof(Transform));
			pTransform->View = viewM;
			pTransform->Proj = projM;
			pTransform->World = terrainWorld;
			const D3D12_GPU_VIRTUAL_ADDRESS cbTerrainDraw = cbGpuBase;
			if (useTerrainDepthPrepass && mainPassDsvCpu.ptr != 0)
			{
				cmdList->OMSetRenderTargets(0, nullptr, FALSE, &mainPassDsvCpu);
				Profiler::GpuMarkTerrainDepthPrepassBegin(cmdList);
				terrainGpuCull->DrawIndirect(
					cmdList,
					terrainRootSignature,
					terrainDepthPrepassPipelineState,
					cbTerrainDraw,
					terrainCB->GetAddress(),
					terrainMaskHandleGPU,
					envCubemapHandleGPU,
					terrainSharedVB,
					terrainSharedIB);
				Profiler::GpuMarkTerrainDepthPrepassEnd(cmdList);
				if (mainPassRtvCpu.ptr != 0)
					cmdList->OMSetRenderTargets(1, &mainPassRtvCpu, FALSE, &mainPassDsvCpu);
				++statTotal;
				++statTerr;
			}
			Profiler::GpuMarkTerrainColorBegin(cmdList);
			terrainGpuCull->DrawIndirect(
				cmdList,
				terrainRootSignature,
				terrainPipelineState,
				cbTerrainDraw,
				terrainCB->GetAddress(),
				terrainMaskHandleGPU,
				envCubemapHandleGPU,
				terrainSharedVB,
				terrainSharedIB,
				shadowMapSrvGpu,
				shadowCBGpu);
			Profiler::GpuMarkTerrainColorEnd(cmdList);
			++statTotal;
			++statTerr;
		}
	}

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
	bool pbrBatchContainsPlayer = false;
	ID3D12PipelineState* pbrBatchPso = nullptr;

	static std::vector<PbrDrawBatch> pbrBatches;
	pbrBatches.clear(); // static で再利用

	auto flushPbrBatchLegacy = [&]()
	{
		if (pbrBatchCount == 0 || !pbrBatchVB || !pbrBatchIB)
			return;
		// Root SRV is already rebased to batch start; instanceID starts from 0 in VS.
		if (pbrBatchContainsPlayer)
			GPU_CMD_BEGIN_EVENT(cmdList, 96, 220, 112, L"Player: PBR batch (DrawIndexedInstanced)");
		if (pbrBatchPso)
			cmdList->SetPipelineState(pbrBatchPso);
		cmdList->DrawIndexedInstanced(pbrBatchIndexCount, pbrBatchCount, 0, 0, 0);
		if (pbrBatchContainsPlayer)
			GPU_CMD_END_EVENT(cmdList);
		++statTotal;
		++statPbrBatches;
		statPbrInstances += pbrBatchCount;
		pbrBatchCount = 0;
		pbrBatchContainsPlayer = false;
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
		b.hasPlayerInstance = pbrBatchContainsPlayer;
		b.pso = pbrBatchPso;
		pbrBatches.push_back(b);
		pbrBatchCount = 0;
		pbrBatchContainsPlayer = false;
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
				//DebugLog("[Render] Transform CB slots exhausted (max=%zu)\n", kPerDrawTransformSlotCount);
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
			if (terrainGpuCull && terrainGpuCull->IsValid())
			{
				const D3D12_GPU_VIRTUAL_ADDRESS payloadSrv = terrainGpuCull->DrawPayloadBufferGpuAddress();
				if (payloadSrv != 0)
					cmdList->SetGraphicsRootShaderResourceView(4, payloadSrv);
			}
			if (shadowMapSrvGpu.ptr != 0)
				cmdList->SetGraphicsRootDescriptorTable(5, shadowMapSrvGpu);
			if (shadowCBGpu != 0)
				cmdList->SetGraphicsRootConstantBufferView(6, shadowCBGpu);

			D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
			D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmdList->IASetVertexBuffers(0, 1, &vbView);
			cmdList->IASetIndexBuffer(&ibView);
			cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, mesh.StartIndexLocation, 0, 0);
			++statTotal;
			++statTerr;

			if (isPlayer)
			{
				//DebugLog("[Player][Draw] OK entity=%u DrawIndexed indices=%u lod=%d pos=(%.3f,%.3f,%.3f)\n",
			//	static_cast<unsigned>(it.entity),
			//	mesh.IndexCount,
			//	lod.CurrentLODLevel,
			//	transform.Position.x, transform.Position.y, transform.Position.z);
			}
			continue;
		}

		// PBR instanced
		const bool sameBatch = pbrBatchCount > 0
			&& pbrBatchMaterialKey == it.materialSortKey
			&& pbrBatchVB == it.vb && pbrBatchIB == it.ib && pbrBatchIndexCount == it.indexCount
			&& pbrBatchPso == it.pso;

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
				pbrBatchPso = it.pso;
			}
			else
			{
				flushPbrBatchLegacy();

				cmdList->SetPipelineState(it.pso);
				pbrBatchPso = it.pso;
				cmdList->SetGraphicsRootSignature(rootSignature->Get());
				cmdList->SetGraphicsRootConstantBufferView(0, sceneConstantsCB->GetAddress());
				cmdList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer->GetAddress());
				cmdList->SetGraphicsRootDescriptorTable(3, mesh.MaterialHandle->HandleGPU);
				if (envCubemapHandleGPU.ptr != 0)
					cmdList->SetGraphicsRootDescriptorTable(4, envCubemapHandleGPU);

				const UINT64 batchBaseOffsetBytes = static_cast<UINT64>(writeCursor) * sizeof(InstanceData);
				cmdList->SetGraphicsRootShaderResourceView(2, pbrInstanceBuffer->GetGPUVirtualAddress() + batchBaseOffsetBytes);
				// TreeIndirectVS-only params: defaults for regular meshes.
				cmdList->SetGraphicsRootShaderResourceView(5, 0);
				const UINT treeDefaults[4] = { 0, 0, 0, 0 };
				cmdList->SetGraphicsRoot32BitConstants(6, 4, treeDefaults, 0);

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
			//DebugLog("[Render] PBR instance ring slice full (max=%zu per frame)\n", kMaxPbrInstancesPerFrame);
			continue;
		}

		DirectX::XMStoreFloat4x4(&pbrInstanceMapped[writeCursor].World, transform.WorldMatrix);
		pbrInstanceMapped[writeCursor].materialIndex = mesh.BindlessMaterialIndex;
		pbrInstanceMapped[writeCursor]._pad[0] = pbrInstanceMapped[writeCursor]._pad[1] = pbrInstanceMapped[writeCursor]._pad[2] = 0;
		++writeCursor;
		++pbrBatchCount;
		if (EntityIsPlayerFamilyMesh(registry, it.entity))
			pbrBatchContainsPlayer = true;

		if (const auto* link = registry.try_get<ModelGroupChildComponent>(it.entity))
		{
			if (registry.valid(link->parent) && registry.all_of<PlayerComponent>(link->parent))
				++statPlayerSub;
		}

		//if (isPlayer)
		//{
		//	DebugLog("[Player][Draw] OK entity=%u instanced indices=%u lod=%d pos=(%.3f,%.3f,%.3f)\n",
		//		static_cast<unsigned>(it.entity),
		//		mesh.IndexCount,
		//		lod.CurrentLODLevel,
		//		transform.Position.x, transform.Position.y, transform.Position.z);
		//}
	}

	if (parallelPbrRecord)
	{
		flushPbrBatchToVector();
		const size_t n = pbrBatches.size();
		const size_t mid = (n + 1) / 2;
		uint32_t d0 = 0, inst0 = 0, d1 = 0, inst1 = 0;
		std::thread worker0([&]()
		{
			GPU_CMD_BEGIN_EVENT(cmdListPbrRecord0, 120, 200, 255, L"RenderSystem: PBR record [0]");
			RecordPbrBatchesOnCmd(cmdListPbrRecord0, sharedSrvHeapForPbr, pbrBatches, 0, mid,
				rootSignature, pipelineState, mainPassRtvCpu, mainPassDsvCpu, d0, inst0);
			GPU_CMD_END_EVENT(cmdListPbrRecord0);
		});
		std::thread worker1([&]()
		{
			GPU_CMD_BEGIN_EVENT(cmdListPbrRecord1, 120, 200, 255, L"RenderSystem: PBR record [1]");
			RecordPbrBatchesOnCmd(cmdListPbrRecord1, sharedSrvHeapForPbr, pbrBatches, mid, n,
				rootSignature, pipelineState, mainPassRtvCpu, mainPassDsvCpu, d1, inst1);
			GPU_CMD_END_EVENT(cmdListPbrRecord1);
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

	s_instanceRingWriteEndExclusive = static_cast<uint32_t>(writeCursor);

	s_lastGpuStats.drawIndexedTotal = statTotal;
	s_lastGpuStats.terrainDraws = statTerr;
	s_lastGpuStats.pbrBatchDrawCalls = statPbrBatches;
	s_lastGpuStats.pbrInstancesDrawn = statPbrInstances;
	s_lastGpuStats.playerModelSubmeshDraws = statPlayerSub;
	s_lastGpuStats.pbrFrustumCulledEntities = statPbrFrustumCulled;
	s_lastGpuStats.treeTagEntityCount = statTreeTagCount;
	s_lastGpuStats.treeEntitiesInDrawQueue = treeInDrawQueue;
	s_lastGpuStats.treeFrustumCulled = statTreeFrustumCulled;
	s_lastGpuStats.treeGpuIndirectBatches = 0;
	s_lastGpuStats.treeGpuUploadedInstanceCount = 0;
}

void RenderSystem::DrawPostScenePbrTreesExecuteIndirect(
	ID3D12GraphicsCommandList* postCmdList,
	ID3D12DescriptorHeap* materialSrvHeap,
	D3D12_CPU_DESCRIPTOR_HANDLE mainHdrRtvCpu,
	D3D12_CPU_DESCRIPTOR_HANDLE mainDsvCpu,
	TreeGpuCullSystem* treeCull,
	RootSignature* rootSignature,
	PipelineState* psoOpaque,
	PipelineState* psoLeavesAlphaCut,
	PipelineState* psoImposterLod1OrNull,
	PipelineState* psoLod0DepthPrepass,
	D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
	D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
	const D3D12_GPU_DESCRIPTOR_HANDLE matBySpeciesByPartByLod[3][3][3],
	const D3D12_GPU_DESCRIPTOR_HANDLE imposterMatTableBySpecies[3],
	D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
	VertexBuffer* vbByPartByLod[3][3],
	IndexBuffer* ibByPartByLod[3][3],
	const uint32_t indexCountByPartByLod[3][3],
	VertexBuffer* vbImposterQuad,
	IndexBuffer* ibImposterQuad)
{
	if (!postCmdList || !materialSrvHeap || !treeCull || !treeCull->IsValid() || !rootSignature || !rootSignature->IsValid())
		return;
	postCmdList->SetDescriptorHeaps(1, &materialSrvHeap);
	postCmdList->OMSetRenderTargets(1, &mainHdrRtvCpu, FALSE, &mainDsvCpu);
	Profiler::GpuMarkTreeDrawBegin(postCmdList);
	GPU_CMD_BEGIN_EVENT(postCmdList, 120, 220, 120, L"Scene: DrawMain — PBR trees (ExecuteIndirect, post CL)");
	treeCull->DrawIndirectLods(
		postCmdList,
		rootSignature,
		psoOpaque,
		psoLeavesAlphaCut,
		psoImposterLod1OrNull,
		psoLod0DepthPrepass,
		sceneCbGpu,
		materialCbGpu,
		matBySpeciesByPartByLod,
		imposterMatTableBySpecies,
		iblTable,
		vbByPartByLod,
		ibByPartByLod,
		indexCountByPartByLod,
		vbImposterQuad,
		ibImposterQuad);
	{
		const uint32_t ei = treeCull->GetLastDrawIndirectBatchCount();
		const uint32_t pool = treeCull->GetInstanceCount();
		s_lastGpuStats.drawIndexedTotal += ei;
		s_lastGpuStats.treeGpuIndirectBatches = ei;
		s_lastGpuStats.treeGpuUploadedInstanceCount = pool;
	}
	Profiler::GpuMarkTreeDrawEnd(postCmdList);
	GPU_CMD_END_EVENT(postCmdList);
}

bool RenderSystem::GetFrustumCullPbrEnabled()
{
	return s_frustumCullPbrEnabled;
}

void RenderSystem::SetFrustumCullPbrEnabled(bool enabled)
{
	s_frustumCullPbrEnabled = enabled;
}

void RenderSystem::DrawNprPasses(
	entt::registry& registry,
	ID3D12GraphicsCommandList* cmdList,
	ConstantBuffer* sceneConstantsCB,
	ConstantBuffer* pbrPropertyBuffer,
	ID3D12Resource* pbrInstanceBuffer,
	InstanceData* pbrInstanceMapped,
	UINT instanceRingFrameIndex,
	RootSignature* rootSignature,
	PipelineState* nprOpaquePso,
	PipelineState* nprTransparentPso,
	DescriptorHeap* descriptorHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE envCubemapHandleGPU,
	ID3D12DescriptorHeap* materialHeap,
	D3D12_CPU_DESCRIPTOR_HANDLE mainPassRtvCpu,
	D3D12_CPU_DESCRIPTOR_HANDLE mainPassDsvCpu,
	const DirectX::XMFLOAT3& cameraWorldPos)
{
	(void)descriptorHeap;
	using namespace DirectX;

	if (!cmdList || !rootSignature || !sceneConstantsCB || !pbrPropertyBuffer || !pbrInstanceBuffer || !pbrInstanceMapped)
		return;
	const bool haveOpaque = nprOpaquePso && nprOpaquePso->IsValid();
	const bool haveTrans = nprTransparentPso && nprTransparentPso->IsValid();
	if (!haveOpaque && !haveTrans)
		return;

	struct NprItem
	{
		entt::entity e{};
		UINT64 materialKey = 0;
	};
	std::vector<NprItem> opaqueList;
	std::vector<NprItem> transList;
	opaqueList.reserve(64);
	transList.reserve(32);

	for (auto entity : registry.view<TransformComponent, MeshRendererComponent, LODComponent>())
	{
		const auto& lod = registry.get<LODComponent>(entity);
		if (lod.CurrentLODLevel == 3)
			continue;
		if (!registry.all_of<ModelGroupChildComponent>(entity))
			continue;
		const auto& ch = registry.get<ModelGroupChildComponent>(entity);
		if (!registry.valid(ch.parent) || !registry.all_of<NPRTag>(ch.parent))
			continue;
		const auto& mesh = registry.get<MeshRendererComponent>(entity);
		if (!mesh.pVB || !mesh.pIB || !mesh.MaterialHandle)
			continue;
		NprItem it{ entity, mesh.MaterialHandle->HandleGPU.ptr };
		if (mesh.NprTransparent)
			transList.push_back(it);
		else
			opaqueList.push_back(it);
	}

	if (opaqueList.empty() && transList.empty())
		return;

	std::sort(opaqueList.begin(), opaqueList.end(), [](const NprItem& a, const NprItem& b) {
		return a.materialKey < b.materialKey;
	});

	XMVECTOR cam = XMLoadFloat3(&cameraWorldPos);
	std::vector<std::pair<float, entt::entity>> transSorted;
	transSorted.reserve(transList.size());
	for (const auto& it : transList)
	{
		const auto& t = registry.get<TransformComponent>(it.e);
		// WorldMatrix は転置済み。平行移動は r[3] ではなく _14,_24,_34（r[0..2].w）
		XMVECTOR pos = XMVectorSet(
			XMVectorGetW(t.WorldMatrix.r[0]),
			XMVectorGetW(t.WorldMatrix.r[1]),
			XMVectorGetW(t.WorldMatrix.r[2]),
			0.f);
		const float dsq = XMVectorGetX(XMVector3LengthSq(pos - cam));
		transSorted.push_back({ dsq, it.e });
	}
	std::sort(transSorted.begin(), transSorted.end(), [](const std::pair<float, entt::entity>& a,
				  const std::pair<float, entt::entity>& b) {
		return a.first > b.first;
	});

	const UINT sliceBase = static_cast<UINT>(instanceRingFrameIndex * kMaxPbrInstancesPerFrame);
	const UINT sliceEnd = sliceBase + static_cast<UINT>(kMaxPbrInstancesPerFrame);
	uint32_t writeCursor = s_instanceRingWriteEndExclusive;

	ID3D12DescriptorHeap* heaps[] = { materialHeap };
	cmdList->SetDescriptorHeaps(1, heaps);
	if (mainPassRtvCpu.ptr != 0 && mainPassDsvCpu.ptr != 0)
		cmdList->OMSetRenderTargets(1, &mainPassRtvCpu, FALSE, &mainPassDsvCpu);
	cmdList->SetGraphicsRootSignature(rootSignature->Get());
	cmdList->SetGraphicsRootConstantBufferView(0, sceneConstantsCB->GetAddress());
	cmdList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer->GetAddress());

	// RenderDoc: コマンド名は「DrawIndexedInstanced」（D3D12 には非インスタンス版を使っていない）
	GPU_CMD_BEGIN_EVENT(cmdList, 255, 140, 160, L"NPR draws: opaque (DrawIndexedInstanced)");
	auto drawOne = [&](entt::entity entity, PipelineState* pso) -> bool {
		if (!pso || !pso->IsValid())
			return false;
		if (writeCursor >= sliceEnd)
		{
			//DebugLog("[Render] NPR instance ring full (max=%zu per frame)\n", kMaxPbrInstancesPerFrame);
			return false;
		}
		const bool playerPix = EntityIsPlayerFamilyMesh(registry, entity);
		if (playerPix)
			GPU_CMD_BEGIN_EVENT(cmdList, 96, 220, 112, L"Player: NPR DrawIndexedInstanced");
		const auto& transform = registry.get<TransformComponent>(entity);
		const auto& mesh = registry.get<MeshRendererComponent>(entity);
		XMStoreFloat4x4(&pbrInstanceMapped[writeCursor].World, transform.WorldMatrix);
		{
			pbrInstanceMapped[writeCursor].materialIndex = mesh.BindlessMaterialIndex;
			pbrInstanceMapped[writeCursor]._pad[0] = pbrInstanceMapped[writeCursor]._pad[1] = pbrInstanceMapped[writeCursor]._pad[2] = 0;
		}
		const UINT64 batchBaseOffsetBytes = static_cast<UINT64>(writeCursor) * sizeof(InstanceData);
		cmdList->SetPipelineState(pso->Get());
		cmdList->SetGraphicsRootDescriptorTable(3, mesh.MaterialHandle->HandleGPU);
		if (envCubemapHandleGPU.ptr != 0)
			cmdList->SetGraphicsRootDescriptorTable(4, envCubemapHandleGPU);
		cmdList->SetGraphicsRootShaderResourceView(2, pbrInstanceBuffer->GetGPUVirtualAddress() + batchBaseOffsetBytes);
		// TreeIndirectVS-only params: defaults for NPR draws.
		cmdList->SetGraphicsRootShaderResourceView(5, 0);
		const UINT treeDefaults[4] = { 0, 0, 0, 0 };
		cmdList->SetGraphicsRoot32BitConstants(6, 4, treeDefaults, 0);
		D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
		D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->IASetVertexBuffers(0, 1, &vbView);
		cmdList->IASetIndexBuffer(&ibView);
		cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
		++writeCursor;
		if (playerPix)
			GPU_CMD_END_EVENT(cmdList);
		return true;
	};

	for (const auto& it : opaqueList)
	{
		if (!haveOpaque)
			break;
		if (!drawOne(it.e, nprOpaquePso))
			break;
	}
	GPU_CMD_END_EVENT(cmdList);

	GPU_CMD_BEGIN_EVENT(cmdList, 255, 120, 220, L"NPR draws: transparent (DrawIndexedInstanced)");
	for (const auto& pr : transSorted)
	{
		if (!haveTrans)
			break;
		if (!drawOne(pr.second, nprTransparentPso))
			break;
	}
	GPU_CMD_END_EVENT(cmdList);
}
