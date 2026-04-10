#include "SnowParticleSystem.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool SnowParticleSystem::Init(ID3D12Device* device, DescriptorHeap* heap, uint32_t particleCount)
{
	m_particleCount = particleCount;
	if (!CreateBuffers(device, heap)) return false;
	if (!CreateComputePipeline(device)) return false;
	// Render pipeline creation deferred until shaders are compiled
	m_valid = true;
	printf("SnowParticleSystem::Init: %u particles\n", m_particleCount);
	return true;
}

void SnowParticleSystem::Shutdown()
{
	if (m_computeCB && m_computeCBMapped) { m_computeCB->Unmap(0, nullptr); m_computeCBMapped = nullptr; }
	m_particleBuffer.Reset();
	m_computeRootSig.Reset(); m_computePso.Reset(); m_computeCB.Reset();
	m_renderRootSig.Reset(); m_renderPso.Reset();
	m_valid = false;
}

bool SnowParticleSystem::CreateBuffers(ID3D12Device* device, DescriptorHeap* heap)
{
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto uploadProp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	UINT64 bufSize = m_particleCount * sizeof(SnowParticleGpu);

	// Particle buffer (UAV for CS, SRV for VS)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(bufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		auto hr = device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_particleBuffer));
		if (FAILED(hr)) return false;
		m_bufferState = D3D12_RESOURCE_STATE_COMMON;

		// Initial data: spawn all particles at camera position (will be overwritten first frame)
		ComPtr<ID3D12Resource> uploadBuf;
		auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));
		void* mapped = nullptr;
		D3D12_RANGE r{0,0};
		uploadBuf->Map(0, &r, &mapped);
		auto* particles = static_cast<SnowParticleGpu*>(mapped);
		for (uint32_t i = 0; i < m_particleCount; ++i)
		{
			particles[i].position = XMFLOAT3(0, 100, 0);
			particles[i].lifetime = 0; // will respawn immediately
			particles[i].velocity = XMFLOAT3(0, -1.5f, 0);
			particles[i].size = 0.02f;
		}
		uploadBuf->Unmap(0, nullptr);
		// Upload will happen on first Update when CS respawns all dead particles

		// SRV (for VS rendering)
		m_particleSrvIdx = heap->AllocateIndex();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = m_particleCount;
		srv.Buffer.StructureByteStride = sizeof(SnowParticleGpu);
		heap->CreateSRVAt(m_particleSrvIdx, m_particleBuffer.Get(), srv);

		// UAV (for CS update)
		m_particleUavIdx = heap->AllocateIndex();
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = m_particleCount;
		uav.Buffer.StructureByteStride = sizeof(SnowParticleGpu);
		heap->CreateUAVAt(m_particleUavIdx, m_particleBuffer.Get(), uav);
	}

	// Compute constants
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_computeCB));
		D3D12_RANGE r{0,0};
		m_computeCB->Map(0, &r, &m_computeCBMapped);
	}

	return true;
}

bool SnowParticleSystem::CreateComputePipeline(ID3D12Device* device)
{
	// Root signature: CBV b0, UAV u0 (particle buffer)
	CD3DX12_ROOT_PARAMETER1 params[2]{};
	params[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
	CD3DX12_DESCRIPTOR_RANGE1 uavRange{};
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	params[1].InitAsDescriptorTable(1, &uavRange);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init_1_1(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> rsBlob, rsErr;
	auto hr = D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
		rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
		IID_PPV_ARGS(m_computeRootSig.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> csBlob;
	hr = D3DReadFileToBlob(L"SnowUpdate_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
		hr = D3DReadFileToBlob(L"Shaders\\SnowUpdate_CS.cso", csBlob.GetAddressOf());
	if (FAILED(hr))
	{
		printf("SnowParticleSystem: SnowUpdate_CS.cso not found (compile later)\n");
		return true; // non-fatal
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = m_computeRootSig.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_computePso.ReleaseAndGetAddressOf()));
	return SUCCEEDED(hr);
}

void SnowParticleSystem::Update(ID3D12GraphicsCommandList* cmd,
	const XMFLOAT3& cameraPos, float deltaTime, float totalTime)
{
	if (!m_valid || !m_computePso || !cmd) return;

	// Fill constants
	struct alignas(16) SnowCB {
		XMFLOAT4 CameraPos;
		float DeltaTime;
		float Time;
		float SpawnRadius;
		float SpawnHeight;
		float MaxLifetime;
		float MinSize;
		float MaxSize;
		uint32_t ParticleCount;
		XMFLOAT4 WindParams;
		float GravityY;
		float _pad[3];
	};

	if (m_computeCBMapped)
	{
		auto* cb = static_cast<SnowCB*>(m_computeCBMapped);
		cb->CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0);
		cb->DeltaTime = deltaTime;
		cb->Time = totalTime;
		cb->SpawnRadius = settings.spawnRadius;
		cb->SpawnHeight = settings.spawnHeight;
		cb->MaxLifetime = settings.maxLifetime;
		cb->MinSize = settings.minSize;
		cb->MaxSize = settings.maxSize;
		cb->ParticleCount = m_particleCount;
		cb->WindParams = XMFLOAT4(settings.windStrX, settings.windStrZ,
			settings.turbulenceScale, settings.turbulenceStr);
		cb->GravityY = settings.gravityY;
	}

	// Transition to UAV
	if (m_bufferState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuffer.Get(),
			m_bufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->ResourceBarrier(1, &b);
		m_bufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	// Dispatch compute
	cmd->SetComputeRootSignature(m_computeRootSig.Get());
	cmd->SetComputeRootConstantBufferView(0, m_computeCB->GetGPUVirtualAddress());
	// UAV descriptor table — need to set the heap that contains our UAV
	// For now, this requires the scene descriptor heap to be set by the caller
	// cmd->SetComputeRootDescriptorTable(1, uavGpuHandle);

	uint32_t groups = (m_particleCount + 255) / 256;
	cmd->SetPipelineState(m_computePso.Get());
	cmd->Dispatch(groups, 1, 1);

	// UAV barrier
	auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_particleBuffer.Get());
	cmd->ResourceBarrier(1, &uavBarrier);
}

void SnowParticleSystem::Draw(ID3D12GraphicsCommandList* cmd)
{
	if (!m_valid || !m_renderPso || !cmd) return;

	// Transition to SRV for vertex shader
	if (m_bufferState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
	{
		auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuffer.Get(),
			m_bufferState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		cmd->ResourceBarrier(1, &b);
		m_bufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}

	// Draw instanced: 4 vertices per billboard, particleCount instances
	cmd->SetPipelineState(m_renderPso.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	cmd->DrawInstanced(4, m_particleCount, 0, 0);
}
