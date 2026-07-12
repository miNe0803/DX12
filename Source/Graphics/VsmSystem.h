#pragma once
#include "ComPtr.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class DescriptorHeap;

// ============================================================
//  VsmSystem — Virtual Shadow Maps（太陽・クリップマップ方式）本体。
//  カメラ中心のクリップマップ(Step1)を「仮想ページ＋物理プール」で per-pixel 解像度化する。
//  役割: 影の細部エイリアス/継ぎ目を根絶（固定解像度CSMの限界を超える）。
//
//  段階実装（docs/Shadow_GI_RT_master_plan.md §12）:
//   V1(このファイル): リソース＋アドレッシング（描画/サンプルはまだ）
//   V2: 深度からページ要求(CS)  V3: 物理割当＋ページ描画  V4: サンプラ差し替え
//   V5: ページキャッシュ        V6: ローカルライト＋ボリューメトリック
//
//  ライト空間クリップマップ: worldPos を lightView で光空間へ→ XY 平面をレベル毎に
//  異なる世界範囲(baseExtent*2^level)で覆う。全レベル同一仮想解像度=近いほど高精細。
//  物理ページ(128²)だけが実メモリを持つ（疎）。ページテーブルが仮想→物理を対応付ける。
// ============================================================
class VsmSystem
{
public:
    static constexpr uint32_t kLevels             = 8;    // クリップマップレベル数
    static constexpr uint32_t kPageSize           = 128;  // 1ページの解像度(texel)
    static constexpr uint32_t kVirtualPagesPerRow = 64;   // 仮想=64*128=8192² / レベル
    static constexpr uint32_t kAtlasPagesPerRow   = 64;   // 物理アトラス=64*128=8192²
    static constexpr float    kBaseExtent         = 4.0f; // レベル0の世界範囲(m)。level i = 4*2^i
    static constexpr uint32_t kVirtualPagesPerLevel = kVirtualPagesPerRow * kVirtualPagesPerRow;
    static constexpr uint32_t kTotalVirtualPages    = kLevels * kVirtualPagesPerLevel; // 32768
    static constexpr uint32_t kPhysicalPages        = kAtlasPagesPerRow * kAtlasPagesPerRow; // 4096
    static constexpr uint32_t kInvalidPage          = 0xFFFFu;

    bool Init(ID3D12Device* device, DescriptorHeap* sceneHeap, uint32_t width, uint32_t height);
    void Shutdown();
    bool IsValid() const { return m_valid; }

    // 毎フレーム: 太陽ライト視点＋カメラ位置からクリップマップ定数を更新（レベル中心をページ格子へスナップ）。
    void UpdateConstants(const DirectX::XMMATRIX& lightView, const DirectX::XMMATRIX& invViewProj,
                         const DirectX::XMFLOAT3& camPos, float lightZNear, float lightZFar);

    // V2: シーン深度から必要な仮想ページを要求バッファへマーク（要求をクリア→CS→読み戻し検証）。
    // depthResource は DEPTH_WRITE 状態で渡す。
    void MarkPages(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depthResource);
    uint32_t LastRequestedPages() const { return m_lastRequestCount; }

    // V3a: 要求ページに物理ページを割当て、ページテーブル(vp→phys)と逆引き(phys→vp)を書く。
    // MarkPages の後に呼ぶ。
    void Allocate(ID3D12GraphicsCommandList* cmd);
    uint32_t LastAllocatedPages() const { return m_lastAllocCount; }

    // V3c-m1: 割当済み各物理ページの描画変換パラメータ(level中心/範囲, px,py, tx,ty)を構築。
    // Allocate の後に呼ぶ。V3c-m3 の per-page 描画で参照。
    void BuildPageParams(ID3D12GraphicsCommandList* cmd);
    ID3D12Resource* GetPageCenterExtent() const { return m_pageCenterExtent.Get(); }
    ID3D12Resource* GetPageTile() const { return m_pageTile.Get(); }

    D3D12_GPU_VIRTUAL_ADDRESS GetConstantsAddress() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetPageTableSrvGpu() const { return m_pageTableSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetAtlasSrvGpu() const { return m_atlasSrvGpu; }
    ID3D12Resource* GetAtlas() const { return m_atlas.Get(); }
    ID3D12Resource* GetPageTable() const { return m_pageTable.Get(); }

    // GPU が参照する定数（Vsm.hlsli と一致させること）
    struct alignas(256) VsmConstants
    {
        DirectX::XMMATRIX LightView;             // world -> light 空間
        DirectX::XMMATRIX InvViewProj;           // clip -> world（深度から復元）
        DirectX::XMFLOAT4 Params;                // x=levelCount, y=pageSize, z=vppr, w=atlasPpr
        DirectX::XMFLOAT4 ZParams;               // x=lightZNear, y=lightZFar, z=camLightX, w=camLightY
        DirectX::XMFLOAT4 DepthDim;              // x=width, y=height, z=1/w, w=1/h
        // 各レベル: xy=光空間XY中心(スナップ済), z=世界範囲(extent), w=world m/texel
        DirectX::XMFLOAT4 LevelCenterExtent[kLevels];
    };

private:
    bool CreateAtlas(ID3D12Device* device);
    bool CreatePageTable(ID3D12Device* device, DescriptorHeap* sceneHeap);
    bool CreateConstantBuffer(ID3D12Device* device);
    bool CreateRequestResources(ID3D12Device* device);
    bool CreateMarkPipeline(ID3D12Device* device);
    bool CreateAllocResources(ID3D12Device* device);
    bool CreateAllocPipeline(ID3D12Device* device);
    bool CreateBuildResources(ID3D12Device* device);
    bool CreateBuildPipeline(ID3D12Device* device);

    bool m_valid = false;
    uint32_t m_w = 0, m_h = 0;

    // 物理深度アトラス（R32_TYPELESS, DSV=D32_FLOAT, SRV=R32_FLOAT）
    ComPtr<ID3D12Resource> m_atlas;
    ComPtr<ID3D12DescriptorHeap> m_atlasDsvHeap;   // アトラス全体の DSV（描画はページ毎ビューポート）
    D3D12_GPU_DESCRIPTOR_HANDLE m_atlasSrvGpu = {};

    // ページテーブル（StructuredBuffer<uint>, 仮想→物理index）。UAV(書込) + SRV(読込)。
    ComPtr<ID3D12Resource> m_pageTable;
    D3D12_GPU_DESCRIPTOR_HANDLE m_pageTableSrvGpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_pageTableUavGpu = {};

    // 定数バッファ（FRAME 毎更新, リング）
    ComPtr<ID3D12Resource> m_cb;
    uint8_t* m_cbMapped = nullptr;
    uint32_t m_cbFrame = 0;
    static constexpr uint32_t kCbFrames = 3;

    // V2: ページ要求バッファ（RWStructuredBuffer<uint>）+ クリア用ゼロbuf + 読戻し検証
    ComPtr<ID3D12Resource> m_requestBuffer;
    ComPtr<ID3D12Resource> m_zeroUpload;               // 毎フレームのクリア元（ゼロ）
    ComPtr<ID3D12Resource> m_requestReadback[kCbFrames];
    ComPtr<ID3D12DescriptorHeap> m_markHeap;           // [0]=depth SRV, [1]=request UAV
    UINT m_markStride = 0;
    ComPtr<ID3D12RootSignature> m_markRootSig;
    ComPtr<ID3D12PipelineState> m_markPso;
    uint32_t m_markFrame = 0;
    uint32_t m_lastRequestCount = 0;
    uint32_t m_dbgThrottle = 0;

    // V3a: 物理割当
    ComPtr<ID3D12Resource> m_counter;                  // [0]=割当数
    ComPtr<ID3D12Resource> m_physToVirtual;            // phys -> vp
    ComPtr<ID3D12Resource> m_counterReadback[kCbFrames];
    ComPtr<ID3D12DescriptorHeap> m_allocHeap;          // [0]=Request SRV,[1]=PageTable UAV,[2]=PhysToVirtual UAV,[3]=Counter UAV
    UINT m_allocStride = 0;
    ComPtr<ID3D12RootSignature> m_allocRootSig;
    ComPtr<ID3D12PipelineState> m_allocPso;
    uint32_t m_allocFrame = 0;
    uint32_t m_lastAllocCount = 0;

    // V3c-m1: per-page 描画パラメータ
    ComPtr<ID3D12Resource> m_pageCenterExtent;   // float4/page: cx,cy,extent,level
    ComPtr<ID3D12Resource> m_pageTile;           // uint4/page: px,py,tx,ty
    ComPtr<ID3D12DescriptorHeap> m_buildHeap;    // [0]=PhysToVirtual SRV,[1]=Counter SRV,[2]=PageCenterExtent UAV,[3]=PageTile UAV
    UINT m_buildStride = 0;
    ComPtr<ID3D12RootSignature> m_buildRootSig;
    ComPtr<ID3D12PipelineState> m_buildPso;
};
