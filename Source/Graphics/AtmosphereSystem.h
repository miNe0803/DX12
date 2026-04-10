#pragma once

#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>

class DescriptorHeap;
class ShadowSystem;

struct AtmosphereParams
{
	// Sun
	float sunAzimuth    = 220.0f;   // degrees, 0=North, 90=East
	float sunElevation  = 45.0f;    // degrees above horizon
	float sunIntensity  = 1.2f;
	float sunColorR     = 1.0f;
	float sunColorG     = 1.0f;
	float sunColorB     = 1.0f;

	// Fog
	float fogDensity    = 0.003f;
	float scatteringG   = 0.7f;
	float heightFalloff = 0.002f;
	float baseHeight    = 0.0f;
	float maxFogDistance = 2000.0f;
	float noiseStrength = 0.4f;
	float temporalBlend = 0.05f;
	float fogColorR     = 0.55f;
	float fogColorG     = 0.62f;
	float fogColorB     = 0.78f;
	bool  enableFog     = true;
	bool  enableVolumetric = true;
	bool  enableTemporal = false;
	bool  enableTreeShadows = false;
	int   treeShadowMaxInstances = 128;
	int   treeShadowCascades = 1;
	float treeShadowDistance = 30.0f;
};

/// Exponential height fog + volumetric light (ray-marched at quarter resolution)
/// with temporal reprojection and bilateral upsampling.
class AtmosphereSystem
{
public:
	bool Init(ID3D12Device* device, DescriptorHeap* sceneHeap,
		UINT fullWidth, UINT fullHeight,
		ID3D12Resource* sceneDepthResource);
	void Shutdown();

	bool IsValid() const { return m_valid; }

	/// Dispatch volumetric CS at quarter resolution, then composite fog + vol onto the HDR target.
	void Execute(
		ID3D12GraphicsCommandList* cmd,
		ID3D12DescriptorHeap* sceneDescriptorHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE hdrSceneSrvGpu,
		D3D12_CPU_DESCRIPTOR_HANDLE hdrSceneRtvCpu,
		ID3D12Resource* hdrResource,
		ID3D12Resource* depthResource,
		const ShadowSystem* shadow,
		const DirectX::XMMATRIX& invViewProj,
		const DirectX::XMFLOAT3& cameraPos,
		const DirectX::XMFLOAT3& sunDir,
		const DirectX::XMFLOAT4& sunColor,
		UINT frameIndex,
		const AtmosphereParams& params);

private:
	bool CreateVolumetricResources(ID3D12Device* device, DescriptorHeap* sceneHeap);
	bool CreateVolumetricPipeline(ID3D12Device* device);
	bool CreateTemporalPipeline(ID3D12Device* device);
	bool CreateCompositePipeline(ID3D12Device* device, DescriptorHeap* sceneHeap);
	bool CreateConstantBuffer(ID3D12Device* device);

	bool m_valid = false;
	UINT m_fullW = 0, m_fullH = 0;
	UINT m_quarterW = 0, m_quarterH = 0;

	// Volumetric CS
	ComPtr<ID3D12RootSignature> m_volRootSig;
	ComPtr<ID3D12PipelineState> m_volPso;

	ComPtr<ID3D12Resource> m_volumetricTex;     // current frame raw output
	ComPtr<ID3D12Resource> m_temporalTexA;       // ping-pong buffer A
	ComPtr<ID3D12Resource> m_temporalTexB;       // ping-pong buffer B
	bool m_temporalPingPong = false;             // false = A is prev, B is output; true = flip
	ComPtr<ID3D12DescriptorHeap> m_volDescHeap;
	UINT m_volDescStride = 0;

	// Temporal CS
	ComPtr<ID3D12RootSignature> m_temporalRootSig;
	ComPtr<ID3D12PipelineState> m_temporalPso;
	ComPtr<ID3D12DescriptorHeap> m_temporalDescHeap;  // [0]=depth, [1]=currentVol, [2]=prevVol, [3]=outVol(UAV)
	UINT m_temporalDescStride = 0;

	DirectX::XMMATRIX m_prevViewProj = DirectX::XMMatrixIdentity();
	bool m_hasPrevViewProj = false;

	struct alignas(256) TemporalCBData {
		DirectX::XMMATRIX InvViewProj;
		DirectX::XMMATRIX PrevViewProj;
		DirectX::XMFLOAT4 CameraPos;
		DirectX::XMFLOAT4 TemporalParams; // x=blendAlpha, y=quarterW, z=quarterH, w=frameIndex
	};
	ComPtr<ID3D12Resource> m_temporalCB;
	TemporalCBData* m_temporalCBMapped = nullptr;

	// Composite PS
	ComPtr<ID3D12RootSignature> m_compRootSig;
	ComPtr<ID3D12PipelineState> m_compPso;

	ComPtr<ID3D12DescriptorHeap> m_compDescHeap;
	UINT m_compDescStride = 0;

	// Atmosphere CB
	struct alignas(256) AtmosphereCBData {
		DirectX::XMMATRIX InvViewProj;
		DirectX::XMFLOAT4 CameraPos;
		DirectX::XMFLOAT4 SunDirection;
		DirectX::XMFLOAT4 SunColor;
		DirectX::XMFLOAT4 FogParams;
		DirectX::XMFLOAT4 FrameParams;
		DirectX::XMFLOAT4 FogColor;
	};
	ComPtr<ID3D12Resource> m_cb;
	AtmosphereCBData* m_cbMapped = nullptr;
};
