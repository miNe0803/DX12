#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

// DXR-GI Phase R: レイトレース AO（RTAO）。inline RayQuery で町 TLAS に遮蔽レイを飛ばし、
// 半解像度 R16 に AO を出力→ 静的再投影で時間デノイズ→ GTAO と同じ乗算適用パス
// (ToneMap_VS + GTAOApply_PS) で HDR に乗算。GtaoSystem のクローン構造。
// TLAS が存在する時（DX12_GI=1）だけ生成される。GTAO とは排他。
//   R1: レイトレース + 3x3ブラー乗算合成。
//   R2: + 静的再投影の時間デノイズ（前VP+ピンポン履歴。AtmosphereSystem の手法）。
class RtaoSystem
{
public:
    struct Params
    {
        float radius = 2.0f;       // AO 半球半径 = レイ TMax (world m)。距離減衰と併せ偽遮蔽を抑制
        float normalBias = 0.05f;  // レイ原点の法線押し出し (world m)。コプラナ自己交差回避
        float tMin = 0.03f;        // レイ最小距離 (world m)。近接自己交差スキップ
        int   rayCount = 4;        // 1px あたり遮蔽レイ本数（時間デノイズがあるので少なめでよい）
        float strength = 1.0f;     // AO 強度 0..1
        float blendAlpha = 0.1f;   // 時間混合率（小さいほど滑らか・収束遅い）
    };

    bool Init(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();
    bool IsValid() const { return m_valid; }

    // depthResource: シーン深度 (R32_TYPELESS, DEPTH_WRITE 状態で渡す。呼出し後 DEPTH_WRITE に戻す)。
    // hdrRtvCpu: メイン HDR RTV（乗算適用先, RENDER_TARGET 状態）。
    // tlasGpuVA: RayTracingManager::GetTlasGpuVA()（ルートSRVでバインド）。
    void Execute(ID3D12GraphicsCommandList* cmd,
        ID3D12Resource* depthResource, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvCpu,
        D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA,
        const DirectX::XMMATRIX& invViewProj, const DirectX::XMFLOAT3& cameraPos,
        const Params& params);

private:
    bool m_valid = false;
    UINT m_w = 0, m_h = 0;      // フル解像度（適用パスのビューポート）
    UINT m_aoW = 0, m_aoH = 0;  // AO 計算解像度（半分）
    uint32_t m_frameIndex = 0;

    // 生AO（レイCS出力）+ 履歴ピンポン2枚（デノイズ出力＝次フレームの履歴入力）
    ComPtr<ID3D12Resource> m_rawAo;               // R16_FLOAT half-res, レイCS UAV → デノイズ SRV
    ComPtr<ID3D12Resource> m_history[2];          // R16_FLOAT half-res, ピンポン
    D3D12_RESOURCE_STATES  m_rawState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES  m_histState[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    int m_histWrite = 0;                          // 今フレーム書き込む履歴インデックス（毎フレーム反転）
    DirectX::XMMATRIX m_prevViewProj = DirectX::XMMatrixIdentity();
    bool m_hasPrev = false;

    ComPtr<ID3D12Resource> m_rtCb;                // RtaoCb（レイCS+適用で共有。先頭88BはAoCb互換）
    uint8_t* m_rtCbMapped = nullptr;
    ComPtr<ID3D12Resource> m_dnCb;                // DenoiseCb
    uint8_t* m_dnCbMapped = nullptr;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;       // 6 記述子（下記レイアウト）
    UINT m_srvStride = 0;

    ComPtr<ID3D12RootSignature> m_rtRootSig;      // b0 CBV, t0 TLAS(root SRV), t1 depth(table), u0 rawAO(table)
    ComPtr<ID3D12PipelineState> m_rtPso;          // Rtao_CS
    ComPtr<ID3D12RootSignature> m_dnRootSig;      // b0 CBV, t0..t2(table: depth,rawAO,prevHist), u0 curHist(table)
    ComPtr<ID3D12PipelineState> m_dnPso;          // RtaoDenoise_CS
    ComPtr<ID3D12RootSignature> m_applyRootSig;   // b0 CBV, t0 AO SRV(table)  = GTAO と同一
    ComPtr<ID3D12PipelineState> m_applyPso;       // ToneMap_VS + GTAOApply_PS（乗算ブレンド）

    // 先頭 InvViewProj/CamPos/InvRes のオフセットは GTAO の AoCb と一致
    // （適用パスが GTAOApply_PS を再利用し InvRes をオフセット80で読むため）。
    struct RtaoCb
    {
        DirectX::XMMATRIX InvViewProj;   // 0
        DirectX::XMFLOAT4 CamPos;        // 64
        DirectX::XMFLOAT2 InvRes;        // 80  ← GTAOApply_PS がここを読む
        float Radius;                    // 88
        float NormalBias;                // 92
        float TMin;                      // 96
        int   RayCount;                  // 100
        float Strength;                  // 104
        uint32_t FrameIndex;             // 108
        DirectX::XMFLOAT3 _pad;          // 112..123
    };
    struct DenoiseCb
    {
        DirectX::XMMATRIX InvViewProj;   // 0
        DirectX::XMMATRIX PrevViewProj;  // 64
        DirectX::XMFLOAT2 InvRes;        // 128
        float BlendAlpha;                // 136
        uint32_t HasHistory;             // 140
        DirectX::XMFLOAT4 _pad;          // 144..159
    };
};
