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
/// Phase B+C: GPU frustum culling + ExecuteIndirect for tree shadow instances.
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

	/// (Legacy) Draw all tree instances without GPU culling.
	void DrawTreeShadows(
		ID3D12GraphicsCommandList* cmd,
		UINT cascadeIndex,
		ID3D12Resource* instanceDataBuffer,
		UINT instanceCount,
		VertexBuffer* vb,
		IndexBuffer* ib,
		UINT indexCount,
		ID3D12DescriptorHeap* srvHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE alphaMaskSrvGpu);

	bool HasTreeShadowPipeline() const { return m_treeShadowPso.Get() != nullptr; }

	// ---- GPU Shadow Culling + ExecuteIndirect (Phase B+C) ----

	bool HasShadowCullPipeline() const { return m_shadowCullPso.Get() != nullptr; }

	/// Dispatch frustum cull CS for one cascade. Call before BeginShadowPass.
	/// treeInfoBuffer: TreeGpuCullSystem's TreeInfo GPU resource.
	/// maxOutput: hard cap on InstanceCount in the indirect args (prevents GPU overload).
	/// cameraPos: camera world position for distance culling.
	/// maxShadowDist: trees beyond this XZ distance from camera are excluded (0 = unlimited).
	void DispatchShadowCull(
		ID3D12GraphicsCommandList* cmd,
		UINT cascadeIndex,
		ID3D12Resource* treeInfoBuffer,
		UINT instanceCount,
		UINT maxOutput,
		const DirectX::XMFLOAT3& cameraPos,
		float maxShadowDist);

	/// Pre-set IndexCount for a cascade's indirect args (call before DispatchShadowCull).
	void SetShadowIndexCount(UINT cascadeIndex, UINT indexCount);

	/// Draw GPU-culled tree shadows via ExecuteIndirect. Call between Begin/EndShadowPass.
	void DrawTreeShadowsIndirect(
		ID3D12GraphicsCommandList* cmd,
		UINT cascadeIndex,
		ID3D12Resource* instanceDataBuffer,
		VertexBuffer* vb,
		IndexBuffer* ib,
		ID3D12DescriptorHeap* srvHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE alphaMaskSrvGpu);

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
	bool CreateShadowCullPipeline(ID3D12Device* device);
	bool CreateShadowCullBuffers(ID3D12Device* device, UINT maxInstances);

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
	ComPtr<ID3D12PipelineState> m_treeShadowPso;        // depth-only (no PS)
	ComPtr<ID3D12PipelineState> m_treeShadowAlphaPso;   // with alpha-test PS

	DirectX::XMMATRIX m_lightVP[kCascadeCount] = {};
	float m_cascadeSplits[kCascadeCount] = {};

	D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	static constexpr UINT kPerDrawRingSlots = 4096;
	ComPtr<ID3D12Resource> m_perDrawRing;
	uint8_t* m_perDrawRingMapped = nullptr;
	UINT m_perDrawRingOffset = 0;

	// Identity visible-index buffer [0,1,2,...,N-1] for legacy fallback
	ComPtr<ID3D12Resource> m_identityVisibleIdx;
	UINT m_identitySize = 0;
	bool EnsureIdentityBuffer(ID3D12Device* device, UINT count);

	// ---- GPU Shadow Cull resources ----
	ComPtr<ID3D12RootSignature> m_shadowCullRootSig;
	ComPtr<ID3D12PipelineState> m_shadowCullPso;
	ComPtr<ID3D12CommandSignature> m_shadowCmdSig;

	// Per-cascade: visible index buffer + indirect args
	ComPtr<ID3D12Resource> m_shadowVisibleIdx[kCascadeCount];
	ComPtr<ID3D12Resource> m_shadowIndirectArgs[kCascadeCount];
	D3D12_RESOURCE_STATES m_shadowArgsState[kCascadeCount] = {};
	D3D12_RESOURCE_STATES m_shadowVisIdxState[kCascadeCount] = {};

	ComPtr<ID3D12Resource> m_shadowArgsResetUpload; // zeroed template for CopyBufferRegion
	ComPtr<ID3D12Resource> m_shadowCullCB;          // upload CB for cull CS
	uint8_t* m_shadowCullCBMapped = nullptr;

	// Shader-visible descriptor heap for cull CS (SRV + UAVs)
	ComPtr<ID3D12DescriptorHeap> m_shadowCullDescHeap;
	UINT m_shadowCullDescStride = 0;
	UINT m_shadowCullMaxInstances = 0;
};
