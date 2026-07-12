#pragma once

#include "ComPtr.h"
#include "PostProcessSettings.h"
#include <d3d12.h>

class DescriptorHeap;

class PostProcessSystem
{
public:
	// width/height: HDR 解像度（Bloom 用 RT は半分で作成）
	// nprHdrSceneColor: NPR レイヤー用 HDR（Engine の GetNprHdrColorResource）。Init 内で SRV 登録し分割コンポで使用。
	bool Init(ID3D12Device* device, DescriptorHeap* descriptorHeap, UINT width, UINT height, ID3D12Resource* nprHdrSceneColor);

	/// compositeNprLayer: true かつ NPR HDR SRV 登録済みのとき、PBR を LDR 中間へ → NPR トーン → 合成。
	void Execute(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12DescriptorHeap* sceneDescriptorHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE hdrSrvHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE finalOutputRtvCpu,
		const PostProcessSettings& settings,
		bool compositeNprLayer);

	bool IsValid() const { return m_isValid; }
	/// NPR HDR がシーンヒープに登録できていれば true（分割ポストが有効になりうる）
	bool HasRegisteredNprHdrSrv() const { return m_hasNprHdrSrv; }

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

	// ---- オート露出（HDR平均輝度→1x1縮約→CPU読み戻し→時間平滑）----
	ComPtr<ID3D12RootSignature> m_pAeRootSignature;
	ComPtr<ID3D12PipelineState> m_pAePSO;
	ComPtr<ID3D12Resource>      m_pAeTarget;        // 1x1 R32_FLOAT（平均log2輝度）
	ComPtr<ID3D12DescriptorHeap> m_pAeRtvHeap;      // 1 slot
	ComPtr<ID3D12Resource>      m_pAeReadback[2];   // READBACK（ping-pong）
	UINT   m_aeIndex = 0;
	float  m_smoothedExposure = 1.0f;
	bool   m_aeValid = false;

	// NPR トーン + 最終合成（PBR LDR + NPR LDR）
	ComPtr<ID3D12RootSignature> m_pNprTonemapRootSignature;
	ComPtr<ID3D12PipelineState> m_pNprTonemapPSO;
	ComPtr<ID3D12RootSignature> m_pCompositeRootSignature;
	ComPtr<ID3D12PipelineState> m_pCompositePSO;

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

	// フル解像度 LDR 中間（合成用。t0=PBR, t1=NPR はシーンヒープで連続登録）
	ComPtr<ID3D12Resource> m_pPbrLdr;
	ComPtr<ID3D12Resource> m_pNprLdr;
	ComPtr<ID3D12DescriptorHeap> m_pLdrRtvHeap;
	UINT m_ldrRtvDescriptorSize = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE m_pbrLdrRtvCpu = {};
	D3D12_CPU_DESCRIPTOR_HANDLE m_nprLdrRtvCpu = {};
	D3D12_GPU_DESCRIPTOR_HANDLE m_compositeLayersSrvGpuBase = {};
	bool m_hasNprHdrSrv = false;
	D3D12_GPU_DESCRIPTOR_HANDLE m_nprHdrSrvGpu = {};
};
