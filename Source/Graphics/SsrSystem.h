#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>

// スクリーン空間反射(SSR)による「濡れた水たまり」。
// HDR をコピーし、指定した平面水たまり領域を反射面として深度レイマーチで
// シーン色を映す。AtmosphereSystem の合成パスと同じ枠組み（フルスクリーン
// 三角形 + 深度SRV）で、HiZ Build 後・Atmosphere 前に Execute する。
class SsrSystem
{
public:
    // 水たまり領域＋レイマーチのパラメータ（Scene から設定, world 単位=m）。
    struct PuddleParams
    {
        bool  enabled = false;
        DirectX::XMFLOAT2 center{ 0, 0 };   // 水たまり中心 XZ (world)
        float groundY = 0.0f;               // 参考用（現状シェーダは水平面ゲートで判定）
        DirectX::XMFLOAT2 half{ 6.0f, 8.0f };// 半径 XZ (m)
        float edgeFalloff = 1.5f;           // 縁のソフトさ (m)
        float wetDarken = 0.6f;             // 濡れて暗くする係数
        float stride = 0.25f;               // レイマーチ 1 歩 (m)
        float steps = 40.0f;                // レイマーチ歩数
        float thickness = 6.0f;             // 厚み帯 = stride*thickness
        float edgeFade = 0.1f;              // 画面端フェード
        DirectX::XMFLOAT3 skyTint{ 0.42f, 0.52f, 0.68f }; // ミス時の空フォールバック
        float reflectivity = 1.0f;          // 反射強度
    };

    bool Init(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();
    bool IsValid() const { return m_valid; }

    // HiZ Build 後・Atmosphere 前に呼ぶ。HDR を RENDER_TARGET のまま返す。
    // prefilterCube = 環境反射フォールバック用の TextureCube（現状シェーダ未使用だが互換で保持）。
    // reflectionColor = 平面反射パスが描いた町の反射カラー RT（水たまりが画面UVでサンプル）。
    void Execute(ID3D12GraphicsCommandList* cmd,
                 ID3D12Resource* hdrResource,
                 D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvCpu,
                 ID3D12Resource* depthResource,
                 ID3D12Resource* prefilterCube,
                 ID3D12Resource* reflectionColor,
                 ID3D12Resource* puddleNoise,   // UE5 T_blend_noise_a（水たまり分布マスク）
                 D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddr,
                 const PuddleParams& puddle);

private:
    bool m_valid = false;
    UINT m_w = 0, m_h = 0;

    ComPtr<ID3D12Resource> m_hdrCopy;                 // HDR コピー（反射のソース）
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;           // [0]=hdrCopy, [1]=depth, [2]=prefilterCube, [3]=reflection, [4]=puddleNoise
    UINT m_srvStride = 0;

    struct alignas(256) SsrCB
    {
        DirectX::XMFLOAT4 P0;
        DirectX::XMFLOAT4 P1;
        DirectX::XMFLOAT4 P2;
        DirectX::XMFLOAT4 Sky;
    };
    ComPtr<ID3D12Resource> m_cb;
    SsrCB* m_cbMapped = nullptr;
};
