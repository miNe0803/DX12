#pragma once

#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class DescriptorHeap;
class TreeGpuCullSystem;
class VertexBuffer;
class IndexBuffer;

/// Directional-light Cascaded Shadow Map system.
/// Manages a Texture2DArray depth target and per-cascade light VP matrices.
class ShadowSystem
{
public:
	static constexpr UINT kCascadeCount = 4;
	static constexpr UINT kShadowMapSize = 2048;

	bool Init(ID3D12Device* device, DescriptorHeap* sceneHeap);
	void Shutdown();

	bool IsValid() const { return m_valid; }

	/// Call once per frame to recompute cascade splits + light VP matrices.
	void UpdateCascades(
		const DirectX::XMMATRIX& cameraView,
		const DirectX::XMMATRIX& cameraProj,
		const DirectX::XMFLOAT3& lightDir,
		float nearClip,
		float farClip);

	/// Begin shadow pass: transition depth to DSV, clear, set viewport.
	void BeginShadowPass(ID3D12GraphicsCommandList* cmd, UINT cascadeIndex);

	/// End shadow pass for a cascade (no-op barrier; batched in EndAllCascades).
	void EndShadowPass(ID3D12GraphicsCommandList* cmd, UINT cascadeIndex);

	/// Transition entire shadow map array to SRV for sampling.
	void TransitionToSRV(ID3D12GraphicsCommandList* cmd);

	/// Light VP matrix for a cascade (pre-transposed for HLSL column-major).
	DirectX::XMMATRIX GetLightVPTransposed(UINT cascade) const;

	ID3D12Resource* GetShadowMapResource() const { return m_shadowMap.Get(); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetShadowMapSrvGpu() const { return m_shadowMapSrvGpu; }

	ID3D12RootSignature* GetShadowRootSignature() const { return m_rootSignature.Get(); }
	ID3D12PipelineState* GetShadowPSO() const { return m_pso.Get(); }

	/// Shadow constant buffer GPU address (contains kCascadeCount LightVP matrices + cascade splits).
	D3D12_GPU_VIRTUAL_ADDRESS GetShadowCBAddress() const;

	/// Write WorldLightVP for a single draw call; returns GPU address to bind as root CBV.
	D3D12_GPU_VIRTUAL_ADDRESS WritePerDrawCB(const DirectX::XMMATRIX& worldLightVP);

	/// Reset per-draw ring offset (call once per frame before shadow pass).
	void ResetPerDrawRing() { m_perDrawRingOffset = 0; }

	/// Draw all tree instances into the shadow map for a given cascade.
	/// Must be called between BeginShadowPass/EndShadowPass for the cascade.
	void DrawTreeShadows(
		ID3D12GraphicsCommandList* cmd,
		UINT cascadeIndex,
		ID3D12Resource* instanceDataBuffer,
		UINT instanceCount,
		VertexBuffer* vb,
		IndexBuffer* ib,
		UINT indexCount);

	bool HasTreeShadowPipeline() const { return m_treeShadowPso.Get() != nullptr; }

	struct alignas(256) ShadowConstants {
		DirectX::XMMATRIX LightVP[kCascadeCount];
		DirectX::XMFLOAT4 CascadeSplits; // .x/.y/.z/.w = view-space far for cascade 0/1/2/3
	};

private:
	bool CreateShadowMap(ID3D12Device* device);
	bool CreatePipeline(ID3D12Device* device);
	bool CreateTreeShadowPipeline(ID3D12Device* device);
	bool CreateConstantBuffer(ID3D12Device* device);
	bool CreatePerDrawRing(ID3D12Device* device);

	bool m_valid = false;

	ComPtr<ID3D12Resource> m_shadowMap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	UINT m_dsvStride = 0;

	D3D12_GPU_DESCRIPTOR_HANDLE m_shadowMapSrvGpu = {};

	ComPtr<ID3D12Resource> m_shadowCB;
	ShadowConstants* m_shadowCBMapped = nullptr;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;

	// Tree shadow pipeline (instanced, reads StructuredBuffer<InstanceData>)
	ComPtr<ID3D12RootSignature> m_treeShadowRootSig;
	ComPtr<ID3D12PipelineState> m_treeShadowPso;

	DirectX::XMMATRIX m_lightVP[kCascadeCount] = {};
	float m_cascadeSplits[kCascadeCount] = {};

	D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	static constexpr UINT kPerDrawRingSlots = 4096;
	ComPtr<ID3D12Resource> m_perDrawRing;
	uint8_t* m_perDrawRingMapped = nullptr;
	UINT m_perDrawRingOffset = 0;
};
