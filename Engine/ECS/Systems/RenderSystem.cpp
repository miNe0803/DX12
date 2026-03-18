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
#include <d3d12.h>
#include <cstdint>

namespace
{
	RenderSystem::GpuDrawStats s_lastGpuStats;
}

RenderSystem::GpuDrawStats RenderSystem::GetLastGpuDrawStats()
{
	return s_lastGpuStats;
}

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

	std::uint8_t* const cbBase = frameTransformCB ? reinterpret_cast<std::uint8_t*>(frameTransformCB->GetPtr()) : nullptr;
	const D3D12_GPU_VIRTUAL_ADDRESS cbGpuBase = frameTransformCB ? frameTransformCB->GetAddress() : 0;
	Transform* const baseT = frameTransformCB ? frameTransformCB->GetPtr<Transform>() : nullptr;
	const DirectX::XMMATRIX viewM = baseT ? baseT[0].View : DirectX::XMMatrixIdentity();
	const DirectX::XMMATRIX projM = baseT ? baseT[0].Proj : DirectX::XMMatrixIdentity();
	size_t drawSlot = 0;
	uint32_t statTotal = 0, statTerr = 0, statPbr = 0, statPlayerSub = 0;

	for (auto entity : view)
	{
		const auto& lod = view.get<LODComponent>(entity);
		if (lod.CurrentLODLevel == 3)
		{
			if (registry.all_of<PlayerComponent>(entity))
				DebugLog("[Player][Draw] SKIP entity=%u (LOD culled level=3)\n", static_cast<unsigned>(entity));
			continue;
		}

		const auto& transform = view.get<TransformComponent>(entity);
		const auto& mesh = view.get<MeshRendererComponent>(entity);
		const bool isPlayer = registry.all_of<PlayerComponent>(entity);

		if (!mesh.pVB || !mesh.pIB || !frameTransformCB)
		{
			if (isPlayer)
			{
				DebugLog("[Player][Draw] SKIP entity=%u (no VB/IB or frame CB) vb=%p ib=%p frameCB=%p\n",
					static_cast<unsigned>(entity),
					static_cast<void*>(mesh.pVB),
					static_cast<void*>(mesh.pIB),
					static_cast<void*>(frameTransformCB));
			}
			continue;
		}

		const bool isTerrain = useTerrain && registry.all_of<TerrainComponent>(entity);
		if (!isTerrain && (!mesh.MaterialHandle || !pbrPropertyBuffer))
		{
			if (isPlayer)
			{
				DebugLog("[Player][Draw] SKIP entity=%u (no material or PBR CB) material=%p pbrCB=%p\n",
					static_cast<unsigned>(entity),
					static_cast<void*>(mesh.MaterialHandle),
					static_cast<void*>(pbrPropertyBuffer));
			}
			continue;
		}

		if (drawSlot >= kPerDrawTransformSlotCount)
		{
			DebugLog("[Render] Transform CB slots exhausted (max=%zu)\n", kPerDrawTransformSlotCount);
			continue;
		}
		Transform* pTransform = reinterpret_cast<Transform*>(cbBase + drawSlot * sizeof(Transform));
		pTransform->View = viewM;
		pTransform->Proj = projM;
		pTransform->World = transform.WorldMatrix;
		const D3D12_GPU_VIRTUAL_ADDRESS cbThisDraw = cbGpuBase + drawSlot * sizeof(Transform);
		++drawSlot;

		if (isTerrain)
		{
			cmdList->SetPipelineState(terrainPipelineState->Get());
			cmdList->SetGraphicsRootSignature(terrainRootSignature->Get());
			cmdList->SetGraphicsRootConstantBufferView(0, cbThisDraw);
			cmdList->SetGraphicsRootConstantBufferView(1, terrainCB->GetAddress());
			cmdList->SetGraphicsRootDescriptorTable(2, terrainMaskHandleGPU);
			if (envCubemapHandleGPU.ptr != 0)
				cmdList->SetGraphicsRootDescriptorTable(3, envCubemapHandleGPU);
		}
		else
		{
			cmdList->SetPipelineState(pipelineState->Get());
			cmdList->SetGraphicsRootSignature(rootSignature->Get());
			cmdList->SetGraphicsRootConstantBufferView(0, cbThisDraw);
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
		++statTotal;
		if (isTerrain)
			++statTerr;
		else
		{
			++statPbr;
			if (const auto* link = registry.try_get<ModelGroupChildComponent>(entity))
			{
				if (registry.valid(link->parent) && registry.all_of<PlayerComponent>(link->parent))
					++statPlayerSub;
			}
		}

		if (isPlayer)
		{
			DebugLog("[Player][Draw] OK entity=%u DrawIndexed indices=%u lod=%d pos=(%.3f,%.3f,%.3f)\n",
				static_cast<unsigned>(entity),
				mesh.IndexCount,
				lod.CurrentLODLevel,
				transform.Position.x, transform.Position.y, transform.Position.z);
		}
	}
	s_lastGpuStats.drawIndexedTotal = statTotal;
	s_lastGpuStats.terrainDraws = statTerr;
	s_lastGpuStats.pbrDraws = statPbr;
	s_lastGpuStats.playerModelSubmeshDraws = statPlayerSub;
}
