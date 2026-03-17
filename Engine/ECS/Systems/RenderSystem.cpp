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
	D3D12_GPU_DESCRIPTOR_HANDLE envCubemapHandleGPU)
{
	auto view = registry.view<TransformComponent, MeshRendererComponent, LODComponent>();

	for (auto entity : view)
	{
		const auto& lod = view.get<LODComponent>(entity);
		if (lod.CurrentLODLevel == 3)
			continue;

		const auto& transform = view.get<TransformComponent>(entity);
		const auto& mesh = view.get<MeshRendererComponent>(entity);

		if (!mesh.pVB || !mesh.pIB || !mesh.MaterialHandle || !frameTransformCB || !pbrPropertyBuffer)
			continue;

		Transform* pTransform = frameTransformCB->GetPtr<Transform>();
		if (pTransform)
			pTransform->World = transform.WorldMatrix;

		cmdList->SetGraphicsRootConstantBufferView(0, frameTransformCB->GetAddress());
		cmdList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer->GetAddress());
		cmdList->SetGraphicsRootDescriptorTable(2, mesh.MaterialHandle->HandleGPU);
		if (envCubemapHandleGPU.ptr != 0)
			cmdList->SetGraphicsRootDescriptorTable(3, envCubemapHandleGPU);

		D3D12_VERTEX_BUFFER_VIEW vbView = mesh.pVB->View();
		D3D12_INDEX_BUFFER_VIEW ibView = mesh.pIB->View();
		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->IASetVertexBuffers(0, 1, &vbView);
		cmdList->IASetIndexBuffer(&ibView);
		cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
	}
}
