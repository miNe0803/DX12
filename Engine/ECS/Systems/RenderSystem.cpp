#include "RenderSystem.h"
#include "../Components.h"
#include "SharedStruct.h"
#include "ConstantBuffer.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "DescriptorHeap.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include <d3d12.h>

void RenderSystem::DrawMain(
	entt::registry& registry,
	ID3D12GraphicsCommandList* cmdList,
	ConstantBuffer* frameTransformCB,
	ConstantBuffer* pbrPropertyBuffer,
	RootSignature* rootSignature,
	PipelineState* pipelineState,
	DescriptorHeap* descriptorHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE envCubemapHandleGPU,
	RootSignature* terrainRootSignature,
	PipelineState* terrainPipelineState,
	ConstantBuffer* terrainCB,
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskHandleGPU)
{
	auto view = registry.view<TransformComponent, MeshRendererComponent, LODComponent>();
	const bool useTerrain = terrainRootSignature && terrainRootSignature->IsValid()
		&& terrainPipelineState && terrainPipelineState->IsValid()
		&& terrainCB && terrainCB->GetAddress() && terrainMaskHandleGPU.ptr != 0;

	for (auto entity : view)
	{
		const auto& lod = view.get<LODComponent>(entity);
		if (lod.CurrentLODLevel == 3)
			continue;

		const auto& transform = view.get<TransformComponent>(entity);
		const auto& mesh = view.get<MeshRendererComponent>(entity);

		if (!mesh.pVB || !mesh.pIB || !frameTransformCB)
			continue;

		Transform* pTransform = frameTransformCB->GetPtr<Transform>();
		if (pTransform)
			pTransform->World = transform.WorldMatrix;

		const bool isTerrain = useTerrain && registry.all_of<TerrainComponent>(entity);

		if (isTerrain)
		{
			cmdList->SetPipelineState(terrainPipelineState->Get());
			cmdList->SetGraphicsRootSignature(terrainRootSignature->Get());
			cmdList->SetGraphicsRootConstantBufferView(0, frameTransformCB->GetAddress());
			cmdList->SetGraphicsRootConstantBufferView(1, terrainCB->GetAddress());
			cmdList->SetGraphicsRootDescriptorTable(2, terrainMaskHandleGPU);
			if (envCubemapHandleGPU.ptr != 0)
				cmdList->SetGraphicsRootDescriptorTable(3, envCubemapHandleGPU);
		}
		else
		{
			if (!mesh.MaterialHandle || !pbrPropertyBuffer)
				continue;
			cmdList->SetPipelineState(pipelineState->Get());
			cmdList->SetGraphicsRootSignature(rootSignature->Get());
			cmdList->SetGraphicsRootConstantBufferView(0, frameTransformCB->GetAddress());
			cmdList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer->GetAddress());
			cmdList->SetGraphicsRootDescriptorTable(2, mesh.MaterialHandle->HandleGPU);
			if (envCubemapHandleGPU.ptr != 0)
				cmdList->SetGraphicsRootDescriptorTable(3, envCubemapHandleGPU);
		}

		D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
		D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->IASetVertexBuffers(0, 1, &vbView);
		cmdList->IASetIndexBuffer(&ibView);
		cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
	}
}
