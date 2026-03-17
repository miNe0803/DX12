#pragma once

#include <entt/entt.hpp>
#include <d3d12.h>

class ConstantBuffer;
class RootSignature;
class PipelineState;
class DescriptorHeap;

namespace RenderSystem
{
	void DrawMain(
		entt::registry& registry,
		ID3D12GraphicsCommandList* cmdList,
		ConstantBuffer* frameTransformCB,
		ConstantBuffer* pbrPropertyBuffer,
		RootSignature* rootSignature,
		PipelineState* pipelineState,
		DescriptorHeap* descriptorHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE envCubemapHandleGPU);
}
