#pragma once

#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>

// 生成済みキューブマップを背景として描画。設計：メッシュは箱・サンプリングは方向ベクトル。ECS移行時は SkyboxRenderSystem として独立可能。
class SkyboxRenderer
{
public:
	SkyboxRenderer() = default;
	~SkyboxRenderer() = default;

	// cubemap: IBLGenerator などで生成したキューブマップリソース（SRV登録は呼び出し側で行う）
	// srvHandle: そのキューブマップの DescriptorHandle（HandleGPU を Draw で使用）
	bool Init(ID3D12Device* device, ID3D12Resource* cubemap, D3D12_GPU_DESCRIPTOR_HANDLE srvHandle);
	void Draw(
		ID3D12GraphicsCommandList* commandList,
		const DirectX::XMMATRIX& view,      // カメラの View（平行移動は除去して使用）
		const DirectX::XMMATRIX& proj
	);

	bool IsValid() const { return m_isValid; }

private:
	bool m_isValid = false;
	D3D12_GPU_DESCRIPTOR_HANDLE m_cubemapSRV = {};
	ComPtr<ID3D12Resource> m_pCubemap; // Draw 前のバリア用（所有は Scene、参照のみ）

	ComPtr<ID3D12RootSignature> m_pRootSignature;
	ComPtr<ID3D12PipelineState> m_pPSO;
	ComPtr<ID3D12Resource> m_pConstantBuffer;
};
