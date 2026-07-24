#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class DescriptorHeap;

// DXR 仕上げ: レイトレース反射（RTR）。各スクリーン画素で深度→ワールド復元し、地面(≈水平)なら
// 水面法線で反射レイを TLAS へ飛ばし、ヒットを DDGI と同じ陰影（太陽+DDGI間接+空）で色付け。
// half-res RGBA16F を出力し、既存 SsrSystem の反射ソースを差し替える（水たまりマスク/合成は不変）。
// 従来の平面ミラー反射（画面内のみ）に対し「画面外ジオメトリも映る」本物のRT反射。
// TLAS 存在時(DX12_GI)のみ生成。既定OFF(DX12_RTR)。DDGI 無効時は太陽+空のみ。
class RtReflectionSystem
{
public:
    // sharedHeap: Scene の共有 DescriptorHeap（bindless VB/IB/base-tex + env/depth/UAV テーブルが1ヒープで解決）。
    bool Init(ID3D12Device* device, DescriptorHeap* sharedHeap, UINT fullW, UINT fullH);
    void Shutdown();
    bool IsValid() const { return m_valid; }
    ID3D12Resource* GetReflectionResource() const { return m_target.Get(); }

    // POST CL で記録。depthResource は DEPTH_WRITE で渡す（呼出し後 DEPTH_WRITE に戻す）。
    // ddgiCbVA/ddgiSHReadVA が 0 のときはダミーをバインドし UseDdgi=0（太陽+空のみ）。
    void Execute(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depthResource,
        D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA, D3D12_GPU_DESCRIPTOR_HANDLE envCubemapGpuHandle,
        D3D12_GPU_VIRTUAL_ADDRESS geomInfoVA, D3D12_GPU_VIRTUAL_ADDRESS instGeoBaseVA,
        D3D12_GPU_VIRTUAL_ADDRESS ddgiCbVA, D3D12_GPU_VIRTUAL_ADDRESS ddgiSHReadVA, bool ddgiReady,
        const DirectX::XMMATRIX& invViewProj, const DirectX::XMFLOAT3& cameraPos,
        const DirectX::XMFLOAT3& sunColorScaled, const DirectX::XMFLOAT3& sunDir, float giIntensity);

private:
    bool m_valid = false;
    UINT m_fullW = 0, m_fullH = 0, m_halfW = 0, m_halfH = 0;
    uint32_t m_frameIndex = 0;

    DescriptorHeap* m_heap = nullptr;                 // 共有ヒープ（非所有）
    ComPtr<ID3D12Resource> m_target;                  // half-res RGBA16F（UAV書き→SRV読み）
    D3D12_RESOURCE_STATES  m_targetState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t m_depthSrvIdx = 0, m_reflUavIdx = 0;     // 共有ヒープ内 index
    D3D12_GPU_DESCRIPTOR_HANDLE m_depthSrvGpu{ 0 }, m_reflUavGpu{ 0 };

    ComPtr<ID3D12Resource> m_cb; uint8_t* m_cbMapped = nullptr;   // RtrCb
    ComPtr<ID3D12Resource> m_dummySH;      // DDGI-off 時の t1（48B）
    ComPtr<ID3D12Resource> m_dummyDdgiCb;  // DDGI-off 時の b1（96B ゼロ）

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // HLSL RtrCB と一致（144B, クリーン16Bロー）。
    struct RtrCb
    {
        DirectX::XMMATRIX InvViewProj;   // 0
        DirectX::XMFLOAT4 CamPos;        // 64
        DirectX::XMFLOAT3 SunDir;        // 80
        float TMin;                      // 92
        DirectX::XMFLOAT4 SunColor;      // 96
        DirectX::XMFLOAT2 InvRes;        // 112
        float NormalBias;                // 120
        float TMax;                      // 124
        float GiIntensity;               // 128
        float GroundNyMin;               // 132
        uint32_t UseDdgi;                // 136
        uint32_t FrameIndex;             // 140
    };
};
