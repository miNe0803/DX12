#pragma once

#include <entt/entt.hpp>
#include <d3d12.h>

class ConstantBuffer;
class RootSignature;
class PipelineState;
class DescriptorHeap;

namespace RenderSystem
{
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
