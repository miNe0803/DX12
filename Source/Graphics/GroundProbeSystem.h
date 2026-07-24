#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

// キャラ接地スナップ: プレイヤーXZから下向きに TLAS へレイ→地面Y を取得（GroundProbe_CS）。
// 地形の無い町シーンで per-XZ の正確な接地を可能にする（道路/歩道/段差の高さに追従）。
// GPU で地面Y を書き→readback で CPU へ（FRAME_BUFFER_COUNT=2 のフェンスを利用し数フレーム遅延で安全に読む）。
// 静的のみ(mask 0x01)をトレースするので動的キャラ自身には当たらない。
class GroundProbeSystem
{
public:
	bool Init(ID3D12Device* device, uint32_t frameCount);
	void Shutdown();
	bool IsValid() const { return m_valid; }

	// TLAS へ下向きレイを撃ち、地面Y を readback[frameIndex] へ。origin はキャラ頭上少し、tMax は探索距離。
	void Execute(ID3D12GraphicsCommandList4* cmd, D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA,
		const DirectX::XMFLOAT3& origin, float tMax, uint32_t frameIndex);

	// readback[frameIndex] を読む（BeginRender でこの index のフェンス待ち後＝2フレーム前の結果が確定済）。
	// ヒット時 true & outGroundY 更新。
	bool Read(uint32_t frameIndex, float& outGroundY) const;

private:
	bool m_valid = false;
	uint32_t m_frameCount = 2;

	ComPtr<ID3D12RootSignature> m_rootSig;
	ComPtr<ID3D12PipelineState> m_pso;

	ComPtr<ID3D12Resource> m_cb;            // GPCB (upload)
	uint8_t* m_cbMapped = nullptr;
	ComPtr<ID3D12Resource> m_out;           // 地面Y 出力（DEFAULT UAV, 16B）
	D3D12_RESOURCE_STATES m_outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	ComPtr<ID3D12Resource> m_readback[3];   // フレーム毎 readback（最大3）
};
