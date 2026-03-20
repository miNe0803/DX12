#pragma once

#include "ComPtr.h"
#include <d3d12.h>
#include <cstdint>
#include <vector>

/// メインパス深度から R32F ミップピラミッドを構築（各ミップは 2x2 の min 深度＝保守的・Forward-Z 向け）
/// GPU オクルージョン / 将来の Hi-Z カリング用の土台。
class HiZSystem
{
public:
	bool Init(ID3D12Device* device, UINT width, UINT height, ID3D12Resource* depthBuffer);
	void Shutdown();

	bool IsValid() const { return m_valid; }
	UINT GetMipCount() const { return m_mipCount; }
	ID3D12Resource* GetPyramidResource() const { return m_hizPyramid.Get(); }

	void SetEnabled(bool e) { m_enabled = e; }
	bool GetEnabled() const { return m_enabled; }

	/// skybox 等で深度テストした直後〜ポストプロセス前に呼ぶ。depthBuffer は Engine の深度リソース。
	void Build(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depthBuffer);

private:
	bool CreatePipelines(ID3D12Device* device);
	void TransitionDepth(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, D3D12_RESOURCE_STATES to);
	void TransitionHiZMip(ID3D12GraphicsCommandList* cmd, UINT mip, D3D12_RESOURCE_STATES to);
	D3D12_CPU_DESCRIPTOR_HANDLE CpuSrvDepth() const;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuSrvHiZ(UINT mip) const;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuUavHiZ(UINT mip) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuSrvDepth() const;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuSrvHiZ(UINT mip) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuUavHiZ(UINT mip) const;

	bool m_valid = false;
	bool m_enabled = true;
	UINT m_w = 0;
	UINT m_h = 0;
	UINT m_mipCount = 1;

	ComPtr<ID3D12Resource> m_hizPyramid;
	ComPtr<ID3D12DescriptorHeap> m_descHeap;
	UINT m_descriptorStride = 0;
	UINT m_slotDepthSrv = 0;
	UINT m_slotHiZSrvBase = 0;
	UINT m_slotHiZUavBase = 0;

	ComPtr<ID3D12Resource> m_paramBuffer;
	void* m_paramMapped = nullptr;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_psoCopy;
	ComPtr<ID3D12PipelineState> m_psoReduce;

	D3D12_RESOURCE_STATES m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	std::vector<D3D12_RESOURCE_STATES> m_hizMipState;

	ID3D12Resource* m_depthResource = nullptr;
};
