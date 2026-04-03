#pragma once

#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <d3d12.h>

#include "SharedStruct.h"

class ConstantBuffer;
class RootSignature;
class PipelineState;
class DescriptorHeap;
class VertexBuffer;
class IndexBuffer;
class TerrainGpuCullSystem;
class TreeGpuCullSystem;

namespace RenderSystem
{
	/// Step A: 1 フレーム分の描画キュー（ソート後の順序）。デバッグ・Step B 用。
	struct RenderQueueEntry
	{
		entt::entity Entity{};
		bool IsTerrain = false;
	};

	/// 直近の DrawMain で構築したキュー（ソート済み）。次の DrawMain まで有効。
	const RenderQueueEntry* GetLastRenderQueue();
	std::size_t GetLastRenderQueueSize();

	/// 直近の Scene::Draw 内 DrawMain で記録（CB 設定＋DrawIndexed が走った回数）
	struct GpuDrawStats
	{
		uint32_t drawIndexedTotal = 0;
		uint32_t terrainDraws = 0;
		/// PBR: DrawIndexedInstanced 呼び出し回数（バッチ数）
		uint32_t pbrBatchDrawCalls = 0;
		/// PBR: 描画したインスタンス総数（メッシュ単位）
		uint32_t pbrInstancesDrawn = 0;
		/// 親に PlayerComponent がある ModelGroup の子メッシュ（キャラ本体のパーツ描画）
		uint32_t playerModelSubmeshDraws = 0;
		/// 直近フレームで視錐台外として PBR キューから省いたエンティティ数
		uint32_t pbrFrustumCulledEntities = 0;
		/// TreeInstanceTag 付きエンティティ数（レジストリ）
		uint32_t treeTagEntityCount = 0;
		/// 上記のうち DrawMain の描画キューに入った本数
		uint32_t treeEntitiesInDrawQueue = 0;
		/// 上記のうち CPU 視錐台で落とした本数
		uint32_t treeFrustumCulled = 0;
		/// 木: Post CL の `DrawPostScenePbrTreesExecuteIndirect`（ExecuteIndirect バッチ数。DrawMain 本体とは別 CL だが同一シーン統計に合算）
		uint32_t treeGpuIndirectBatches = 0;
		/// 木: GPU にアップロードしたインスタンス数（プール上限内の本フレーム本数）
		uint32_t treeGpuUploadedInstanceCount = 0;
	};
	GpuDrawStats GetLastGpuDrawStats();

	/// Post CL 上で HDR メインへマスク木を PBR（ExecuteIndirect）描画する。Scene 側で同一 Post CL に先立って
	/// UpdateInstances と DispatchCull を記録してから呼ぶ（ExecuteIndirect とカリング結果を同一リストで繋ぐ）。
	/// `drawIndexedTotal` へ木の EI 回数を合算する。
	void DrawPostScenePbrTreesExecuteIndirect(
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
		IndexBuffer* ibImposterQuad);

	/// PBR 視錐台カリング（ロードマップ: Hi-Z 前段の CPU オクルージョン）。デフォルト ON。
	bool GetFrustumCullPbrEnabled();
	void SetFrustumCullPbrEnabled(bool enabled);

	/// DrawMain: 森の木（TreeInstanceTag）はマスク GPU 経路と二重にならないようキューから除外（実装内 constexpr）。
	// 地形: perDrawTransformCB（スロットごと World+View+Proj）。PBR: sceneConstantsCB + instance ring。
	// cmdList: 地形のみ。cmdListPbrRecord0/1 と sharedSrvHeapForPbr が非 nullptr のとき PBR を2スレッドで記録（Engine とセットで使用）。
	void DrawMain(
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
		RootSignature* terrainRootSignature = nullptr,
		PipelineState* terrainDepthPrepassPipelineState = nullptr,
		PipelineState* terrainPipelineState = nullptr,
		ConstantBuffer* terrainCB = nullptr,
		D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU = { 0 },
		ID3D12GraphicsCommandList* cmdListPbrRecord0 = nullptr,
		ID3D12GraphicsCommandList* cmdListPbrRecord1 = nullptr,
		ID3D12DescriptorHeap* sharedSrvHeapForPbr = nullptr,
		D3D12_CPU_DESCRIPTOR_HANDLE mainPassRtvCpu = {},
		D3D12_CPU_DESCRIPTOR_HANDLE mainPassDsvCpu = {},
		/// NPR 親の子を PBR から外すのは、対応する NPR パス（不透明／透明）が実際に描画できるときだけ。
		bool nprOpaquePsoValid = false,
		bool nprTransparentPsoValid = false,
		/// Phase 3/5: nullptr ならチャンク毎 CPU 描画。有効時はキューから TerrainMeshTag を除外し ExecuteIndirect。
		TerrainGpuCullSystem* terrainGpuCull = nullptr,
		VertexBuffer* terrainSharedVB = nullptr,
		IndexBuffer* terrainSharedIB = nullptr,
		PipelineState* treeInstancingLod1Pso = nullptr,
		PipelineState* treeInstancingLod2Pso = nullptr);

	/// DrawMain の後。PBR キューから NPR 親子は除外済み。不透明 NPR → 透明 NPR（距離ソート）。
	void DrawNprPasses(
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
		const DirectX::XMFLOAT3& cameraWorldPos);
}
