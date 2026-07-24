#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

// DXR-GI Phase G: DDGI 拡散GI（ワールド空間 SH-L1 irradiance プローブ）。
// 毎フレーム、全プローブへ inline RayQuery で放射輝度を集めSHへ投影→前フレームとEMA混合。
// TownPS が (worldPos, Nw) でプローブ場をサンプルし、偽ambient（sky-tint）を実GIで置換。
// TLAS 存在時（DX12_GI=1）だけ生成。既定OFF（DX12_DDGI）。プローブ格納は StructuredBuffer(SH-L1)
// をルートSRVで町PSへ渡す＝共有ヒープ不要。
//   G-a: 空のみ放射輝度（miss=空, hit=0）。G-b: 太陽バウンス。G-c: 多重バウンス+漏れ抑制。
class DdgiSystem
{
public:
    // C++ ミラー（HLSL DdgiCB と一致, 96 bytes）。TownPS(b5) も同レイアウトを読む。
    struct DdgiCb
    {
        DirectX::XMFLOAT3 gridOrigin;  uint32_t probeCount;
        DirectX::XMFLOAT3 gridSpacing; uint32_t frameIndex;
        DirectX::XMUINT3  gridDims;    float    normalBias;
        DirectX::XMFLOAT3 sunDir;      float    emaAlpha;
        DirectX::XMFLOAT4 sunColor;    // rgb = 太陽色×強度（G-b）
        uint32_t rayCount; float _pad[3];
    };

    bool Init(ID3D12Device* device, const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax);
    void Shutdown();
    bool IsValid() const { return m_valid; }
    bool IsReady() const { return m_valid && m_framesAccum >= 2; }   // ≥2フレーム蓄積後に町がサンプル

    // 毎フレーム: プローブ更新を記録（POST CL 推奨＝町は前フレームのバッファを読む）。
    // envCubemapGpuHandle: s_envCubemapHandle->HandleGPU（共有ヒープの t2-t4 テーブル基底）。
    // sunColorScaled: 太陽色×sunScale（G-b用, G-aは無視）。sunDir: 太陽へ向かう方向。
    void Execute(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* sharedHeap,
        D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA, D3D12_GPU_DESCRIPTOR_HANDLE envCubemapGpuHandle,
        const DirectX::XMFLOAT3& sunColorScaled, const DirectX::XMFLOAT3& sunDir);

    // 町がサンプルする「直近完了」バッファの VA（＝今フレーム書き込む方の反対）。
    D3D12_GPU_VIRTUAL_ADDRESS GetReadBufferVA() const { return m_probeSH[1 - m_write] ? m_probeSH[1 - m_write]->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS GetCbVA() const { return m_cb ? m_cb->GetGPUVirtualAddress() : 0; }

private:
    bool m_valid = false;
    uint32_t m_framesAccum = 0;
    uint32_t m_frameIndex = 0;
    int m_write = 0;   // 今フレーム書き込む履歴インデックス（毎フレーム反転）

    DdgiCb m_params{};   // 静的グリッド params（Init で確定）

    ComPtr<ID3D12Resource> m_probeSH[2];   // SH-L1 ping-pong（DEFAULT, ALLOW_UNORDERED_ACCESS, probeCount*48B）
    D3D12_RESOURCE_STATES  m_shState[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    ComPtr<ID3D12Resource> m_cb;           // DdgiCb (UPLOAD, 永続マップ)
    uint8_t* m_cbMapped = nullptr;

    ComPtr<ID3D12RootSignature> m_rootSig; // b0 CBV, t0 TLAS(rootSRV), t1 prev(rootSRV), u0 cur(rootUAV), table t2-t4 IBL
    ComPtr<ID3D12PipelineState> m_pso;      // Ddgi_ProbeUpdate_CS
};
