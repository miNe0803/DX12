#pragma once

#include "ComPtr.h"
#include "PostProcessSettings.h"
#include <d3d12.h>

class DescriptorHeap;

class PostProcessSystem
{
public:
	// width/height: HDR 解像度（Bloom 用 RT は半分で作成）
	bool Init(ID3D12Device* device, DescriptorHeap* descriptorHeap, UINT width, UINT height);

	void Execute(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12DescriptorHeap* sceneDescriptorHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE hdrSrvHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtvHandle,
		const PostProcessSettings& settings);

	bool IsValid() const { return m_isValid; }

private:
	bool m_isValid = false;
	UINT m_width = 0;
	UINT m_height = 0;
	UINT m_bloomWidth = 0;
	UINT m_bloomHeight = 0;

	// ToneMap（HDR + Bloom 合成）
	ComPtr<ID3D12RootSignature> m_pRootSignature;
	ComPtr<ID3D12PipelineState> m_pToneMapPSO;

	// Bloom Extract
	ComPtr<ID3D12RootSignature> m_pExtractRootSignature;
	ComPtr<ID3D12PipelineState> m_pExtractPSO;

	// Bloom Blur
	ComPtr<ID3D12RootSignature> m_pBlurRootSignature;
	ComPtr<ID3D12PipelineState> m_pBlurPSO;

	// Bloom 用 RT（1/2 解像度）: Extract → BlurA → BlurB
	ComPtr<ID3D12Resource> m_pBloomExtract;
	ComPtr<ID3D12Resource> m_pBloomBlurA;
	ComPtr<ID3D12Resource> m_pBloomBlurB;
	ComPtr<ID3D12DescriptorHeap> m_pBloomRtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_pBloomSrvHeap;
	UINT m_rtvDescriptorSize = 0;
	UINT m_srvDescriptorSize = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE m_bloomExtractSrvGpu = {};
	D3D12_GPU_DESCRIPTOR_HANDLE m_bloomBlurASrvGpu = {};
	D3D12_GPU_DESCRIPTOR_HANDLE m_bloomBlurBSrvGpu = {};
};
