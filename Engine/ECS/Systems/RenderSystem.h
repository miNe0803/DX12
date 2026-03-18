#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <d3d12.h>

class ConstantBuffer;
class RootSignature;
class PipelineState;
class DescriptorHeap;

namespace RenderSystem
{
	/// 直近の Scene::Draw 内 DrawMain で記録（CB 設定＋DrawIndexed が走った回数）
	struct GpuDrawStats
	{
		uint32_t drawIndexedTotal = 0;
		uint32_t terrainDraws = 0;
		uint32_t pbrDraws = 0;
		/// 親に PlayerComponent がある ModelGroup の子メッシュ（キャラ本体のパーツ描画）
		uint32_t playerModelSubmeshDraws = 0;
	};
	GpuDrawStats GetLastGpuDrawStats();

	// 地形用: いずれか null/0 の場合は地形描画を行わない（通常メッシュのみ）
	void DrawMain(
		entt::registry& registry,
		ID3D12GraphicsCommandList* cmdList,
		ConstantBuffer* frameTransformCB,
		ConstantBuffer* pbrPropertyBuffer,
		RootSignature* rootSignature,
		PipelineState* pipelineState,
		DescriptorHeap* descriptorHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE envCubemapHandleGPU,
		RootSignature* terrainRootSignature = nullptr,
		PipelineState* terrainPipelineState = nullptr,
		ConstantBuffer* terrainCB = nullptr,
		D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU = { 0 });
}
