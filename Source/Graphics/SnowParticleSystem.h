#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class DescriptorHeap;

/// GPU snow particle for compute update + instanced rendering.
struct SnowParticleGpu {
	DirectX::XMFLOAT3 position;
	float lifetime;
	DirectX::XMFLOAT3 velocity;
	float size;
};
static_assert(sizeof(SnowParticleGpu) == 32, "SnowParticleGpu must be 32 bytes");

/// GPU-driven snow particle system. Compute shader updates, instanced billboard render.
class SnowParticleSystem
{
public:
	static constexpr uint32_t kDefaultParticleCount = 80000;

	bool Init(ID3D12Device* device, DescriptorHeap* heap, uint32_t particleCount = kDefaultParticleCount);
	void Shutdown();

	/// Update particles via compute shader. Call once per frame before rendering.
	void Update(ID3D12GraphicsCommandList* cmd,
	            const DirectX::XMFLOAT3& cameraPos,
	            float deltaTime, float totalTime);

	/// Render snow particles as instanced billboard quads.
	/// Caller must have set the RTV, DSV, root signature, and descriptor heap.
	void Draw(ID3D12GraphicsCommandList* cmd);

	uint32_t GetParticleCount() const { return m_particleCount; }
	ID3D12Resource* GetParticleBuffer() const { return m_particleBuffer.Get(); }
	uint32_t GetParticleSrvIdx() const { return m_particleSrvIdx; }

	struct SnowSettings {
		float spawnRadius   = 60.0f;   // meters around camera
		float spawnHeight   = 40.0f;   // meters above camera
		float maxLifetime   = 8.0f;
		float minSize       = 0.015f;
		float maxSize       = 0.045f;
		float gravityY      = -1.5f;
		float windStrX      = 2.0f;
		float windStrZ      = 1.5f;
		float turbulenceScale = 0.1f;
		float turbulenceStr = 0.5f;
	};
	SnowSettings settings;

private:
	bool CreateBuffers(ID3D12Device* device, DescriptorHeap* heap);
	bool CreateComputePipeline(ID3D12Device* device);
	bool CreateRenderPipeline(ID3D12Device* device);

	bool m_valid = false;
	uint32_t m_particleCount = 0;

	// Particle buffer (StructuredBuffer<SnowParticleGpu>, UAV for CS, SRV for VS)
	ComPtr<ID3D12Resource> m_particleBuffer;
	uint32_t m_particleSrvIdx = UINT32_MAX;
	uint32_t m_particleUavIdx = UINT32_MAX;

	D3D12_RESOURCE_STATES m_bufferState = D3D12_RESOURCE_STATE_COMMON;

	// Compute pipeline (update)
	ComPtr<ID3D12RootSignature> m_computeRootSig;
	ComPtr<ID3D12PipelineState> m_computePso;
	ComPtr<ID3D12Resource> m_computeCB;
	void* m_computeCBMapped = nullptr;

	// Render pipeline (VS + PS, instanced billboards)
	ComPtr<ID3D12RootSignature> m_renderRootSig;
	ComPtr<ID3D12PipelineState> m_renderPso;
};
