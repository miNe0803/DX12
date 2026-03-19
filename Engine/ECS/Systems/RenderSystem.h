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
	};
	GpuDrawStats GetLastGpuDrawStats();

	// 地形: perDrawTransformCB（スロットごと World+View+Proj）。PBR: sceneConstantsCB + instance ring。
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
		PipelineState* terrainPipelineState = nullptr,
		ConstantBuffer* terrainCB = nullptr,
		D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU = { 0 });
}
