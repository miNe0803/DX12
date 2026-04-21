#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class DescriptorHeap;
class RayTracingManager;

/// Water rendering system.
/// Draws water surfaces using terrain mask data (NatureMask G channel).
/// Supports DXR reflection (optional) and Beer's Law refraction.
class WaterSystem
{
public:
	bool Init(ID3D12Device* device, DescriptorHeap* heap,
	          ID3D12RootSignature* rootSig, uint32_t screenW, uint32_t screenH);
	void Shutdown();

	/// Draw water surfaces. Call after opaque pass, before transparent pass.
	/// sceneColorSrv: SRV of the opaque pass HDR result (for refraction).
	/// depthSrv: SRV of the main depth buffer.
	void Draw(ID3D12GraphicsCommandList* cmd,
	          D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
	          D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
	          D3D12_GPU_VIRTUAL_ADDRESS sceneCBAddress,
	          ID3D12DescriptorHeap* srvHeap,
	          float time);

	struct WaterSettings {
		float surfaceY        = 10.0f;   // world-space water surface height
		float foamWidth       = 1.5f;    // shore foam distance (meters)
		float specPower       = 256.0f;  // sun specular exponent
		float absorptionScale = 0.8f;
		DirectX::XMFLOAT3 absorptionRGB = { 0.46f, 0.09f, 0.06f };
		DirectX::XMFLOAT3 shallowColor  = { 0.1f, 0.3f, 0.4f };
		// Normal scroll params
		DirectX::XMFLOAT2 scroll1Dir   = { 0.7f, 0.7f };
		float scroll1Speed = 0.02f;
		float scroll1Scale = 4.0f;
		DirectX::XMFLOAT2 scroll2Dir   = { -0.5f, 0.86f };
		float scroll2Speed = 0.015f;
		float scroll2Scale = 6.0f;
	};
	WaterSettings settings;

	bool IsValid() const { return m_valid; }

private:
	bool CreatePipeline(ID3D12Device* device, ID3D12RootSignature* rootSig);
	bool CreateConstantBuffer(ID3D12Device* device);

	bool m_valid = false;
	uint32_t m_screenW = 0, m_screenH = 0;

	// Water PSO (separate from main forward pass)
	ComPtr<ID3D12PipelineState> m_waterPso;

	// WaterCB (b1) — upload mapped
	struct WaterCBData {
		DirectX::XMFLOAT4 WaterParams;     // time, surfaceY, foamWidth, specPower
		DirectX::XMFLOAT4 AbsorptionCoeff; // RGB + scale
		DirectX::XMFLOAT4 WaterColor;      // shallow tint
		DirectX::XMFLOAT4 NormalScroll1;   // dir.xy, speed, scale
		DirectX::XMFLOAT4 NormalScroll2;   // dir.xy, speed, scale
	};
	ComPtr<ID3D12Resource> m_waterCB;
	void* m_waterCBMapped = nullptr;
};
