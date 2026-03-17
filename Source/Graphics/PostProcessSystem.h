#pragma once

#include "ComPtr.h"
#include "PostProcessSettings.h"
#include <d3d12.h>

class PostProcessSystem
{
public:
	bool Init(ID3D12Device* device);

	void Execute(
		ID3D12GraphicsCommandList* cmdList,
		D3D12_GPU_DESCRIPTOR_HANDLE hdrSrvHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtvHandle,
		const PostProcessSettings& settings);

	bool IsValid() const { return m_isValid; }

private:
	bool m_isValid = false;
	ComPtr<ID3D12RootSignature> m_pRootSignature;
	ComPtr<ID3D12PipelineState> m_pToneMapPSO;
};
