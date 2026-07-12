#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

// GI トラック G0: スクリーン空間 AO（SSAO/GTAO-lite）。
// 深度から AO を計算(CS)し、HDR に乗算ブレンドで適用(PS)。DXR 不要・自己完結。
class GtaoSystem
{
public:
    struct Params
    {
        float radius = 3.0f;     // サンプル半径スケール
        float strength = 0.9f;   // AO 強度 0..1
        float bias = 0.03f;      // 法線バイアス
        float maxDist = 3.0f;    // 遮蔽レンジ減衰(world m)
    };

    bool Init(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();
    bool IsValid() const { return m_valid; }

    // depthResource: シーン深度 (R32_TYPELESS, DEPTH_WRITE 状態で渡す)。
    // hdrRtvCpu: メイン HDR RTV（乗算適用先, RENDER_TARGET 状態）。
    void Execute(ID3D12GraphicsCommandList* cmd,
        ID3D12Resource* depthResource, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvCpu,
        const DirectX::XMMATRIX& invViewProj, const DirectX::XMFLOAT3& cameraPos,
        const Params& params);

private:
    bool m_valid = false;
    UINT m_w = 0, m_h = 0;      // フル解像度（適用パスのビューポート）
    UINT m_aoW = 0, m_aoH = 0;  // AO 計算解像度（半分。低周波なので十分・~4倍高速）

    ComPtr<ID3D12Resource> m_aoTexture;           // R16_FLOAT UAV+SRV (half-res)
    D3D12_RESOURCE_STATES  m_aoState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> m_cb;                  // AoCb
    uint8_t* m_cbMapped = nullptr;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;       // [0]=depth SRV, [1]=AO UAV, [2]=AO SRV
    UINT m_srvStride = 0;

    ComPtr<ID3D12RootSignature> m_csRootSig;
    ComPtr<ID3D12PipelineState> m_csPso;
    ComPtr<ID3D12RootSignature> m_applyRootSig;
    ComPtr<ID3D12PipelineState> m_applyPso;

    struct AoCb
    {
        DirectX::XMMATRIX InvViewProj;
        DirectX::XMFLOAT4 CamPos;
        DirectX::XMFLOAT2 InvRes;
        float Radius, Strength;
        float Bias, MaxDist;
        DirectX::XMFLOAT2 _pad;
    };
};
