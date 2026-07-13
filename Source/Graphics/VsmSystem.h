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
    static constexpr uint32_t kAtlasPagesPerRow   = 96;   // 物理アトラス=96*128=12288²(≈576MB D32)。VSM主軸化で
                                                          // プール 4096→9216 に拡大。密なグレージング視点(≈4300枚)も余裕で収容し
                                                          // 溢れ破綻を回避（近~中~町全体を VSM、遠景背景のみ CSM）。vppr(仮想)とは独立。
    static constexpr float    kBaseExtent         = 4.0f; // レベル0の世界範囲(m)。level i = 4*2^i
    static constexpr uint32_t kVirtualPagesPerLevel = kVirtualPagesPerRow * kVirtualPagesPerRow;
    static constexpr uint32_t kTotalVirtualPages    = kLevels * kVirtualPagesPerLevel; // 32768
    static constexpr uint32_t kPhysicalPages        = kAtlasPagesPerRow * kAtlasPagesPerRow; // 4096
    static constexpr uint32_t kInvalidPage          = 0xFFFFu;
    // V3c-m2 ビニング
    static constexpr uint32_t kMaxModels            = 1024;        // ユニークキャスタモデル上限（町 maxUniqueMeshes=500）
    static constexpr uint32_t kMaxPairs             = 1u << 20;     // (caster,page) ペア上限=1,048,576（uint2=8MB）

    bool Init(ID3D12Device* device, DescriptorHeap* sceneHeap, uint32_t width, uint32_t height);
    void Shutdown();
    bool IsValid() const { return m_valid; }

    // 毎フレーム: 太陽ライト視点＋カメラ位置からクリップマップ定数を更新（レベル中心をページ格子へスナップ）。
    void UpdateConstants(const DirectX::XMMATRIX& lightView, const DirectX::XMMATRIX& invViewProj,
                         const DirectX::XMFLOAT3& camPos, float lightZNear, float lightZFar);

    // V5a: 前フレームからカメラ/太陽が変化したか（＝アトラス再描画が必要か）。静止時は false→
    // MarkPages〜RenderPages をスキップし保持アトラスを再利用（サンプルは毎フレーム可）。
    bool NeedsRender() const { return m_needsRender; }

    // V5b 永続キャッシュ（DX12_VSM_CACHE）。ON にすると: 静的な町はページが世界固定＝一度描けば永久有効
    // なので、カメラ移動時も「新規に要求されたページ(=クリップマップに新しく入る先端)」だけを描画する
    // （非要求ページの PageTable/アトラスは保持）。全ページ毎フレーム再描画(=移動時198ms)を回避。
    // 有効化/太陽変化時は次回描画で一度だけリセット（PageTable←0xFFFF, Counter←0, アトラス全クリア）。
    void SetCacheMode(bool on);
    bool GetCacheMode() const { return m_cacheMode; }
    void RequestCacheReset() { m_cacheNeedsReset = true; }   // 太陽変化・視点ワープ時など
    // 物理プール使用量（FIFOリングなので実使用は cap で頭打ち）。m_lastAllocCount は単調 seq なので clamp。
    uint32_t LastResidentPages() const { return m_lastAllocCount < kPhysicalPages ? m_lastAllocCount : kPhysicalPages; }

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

    // V3c-m2: 町の静的キャスタレコード + submesh バッチ表（TownScene が用意）を登録（init 時1回）。
    void SetCasterSource(D3D12_GPU_VIRTUAL_ADDRESS casterVA, uint32_t casterCount, uint32_t modelCount,
                         D3D12_GPU_VIRTUAL_ADDRESS submeshTableVA, uint32_t batchCount);
    // V3c-m2b-e: キャスタ→ページ展開→prefix-sum→scatter→間接引数生成。BuildPageParams 後。純 compute。
    void BuildCasterBinning(ID3D12GraphicsCommandList* cmd);
    uint32_t LastPairCount() const { return m_lastPairCount; }
    ID3D12Resource* GetInstancePairs() const { return m_instancePairs.Get(); }

    // V3c-m3: per-page 深度をアトラスへ描画（ExecuteIndirect × submesh バッチ）。BuildCasterBinning 後。
    struct RenderBatch { D3D12_VERTEX_BUFFER_VIEW vbv; D3D12_INDEX_BUFFER_VIEW ibv; };
    void RenderPages(ID3D12GraphicsCommandList* cmd, const RenderBatch* batches, uint32_t count);

    // 検証: 物理アトラスをフルスクリーン表示（HDR RTV へ上書き）。DX12_VSM_ATLAS 時に呼ぶ。
    void RenderAtlasDebug(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv);
    // 検証(V4前): シーン深度→worldPos→VSMサンプルで影係数を画面出力。DX12_VSM_SHADOW 時。
    void RenderShadowDebug(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
                           ID3D12Resource* sceneDepth);

    D3D12_GPU_VIRTUAL_ADDRESS GetConstantsAddress() const;
    // V5b Stage0: 現在常駐しているアトラスを描画した時の定数(中心)アドレス。町サンプルはこれを使うと、
    // 「前フレーム中心で描いたアトラスを今フレーム中心でサンプル」する不一致(=WSAD移動の揺れ)が消える。
    D3D12_GPU_VIRTUAL_ADDRESS GetRenderedConstantsAddress() const { return m_renderedCBAddr ? m_renderedCBAddr : GetConstantsAddress(); }
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
        // V5b: 各レベル xy=窓原点(整数ページ座標 originX/Y), z=pageWorld(1ページ世界幅), w=world m/texel
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
    bool CreateBinningResources(ID3D12Device* device);
    bool CreateBinningPipelines(ID3D12Device* device);
    bool CreateDrawArgsPipeline(ID3D12Device* device);
    bool CreatePageRenderPipeline(ID3D12Device* device);
    bool CreateClearTilesPipeline(ID3D12Device* device);   // V5b: dirty タイル深度クリア
    bool CreateAtlasDebugPipeline(ID3D12Device* device);
    bool CreateShadowDebugPipeline(ID3D12Device* device);
    void ZeroBuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, uint64_t bytes);

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
    D3D12_GPU_VIRTUAL_ADDRESS m_renderedCBAddr = 0;   // V5b Stage0: 現常駐アトラスを描いた時のCB

    // V5a: 再描画要否（カメラ/太陽の変化検出）
    bool m_needsRender = true;
    bool m_hasLastView = false;
    DirectX::XMFLOAT4X4 m_lastLightView = {};
    DirectX::XMFLOAT4X4 m_lastInvViewProj = {};

    // V4: atlas/pageTable 状態追跡。描画中は working、描画後は PIXEL_SHADER_RESOURCE(resting) へ戻し、
    // 翌フレームの町メインパスがサンプルできるようにする（cross-frame / cross-command-list）。
    D3D12_RESOURCE_STATES m_atlasState = D3D12_RESOURCE_STATE_DEPTH_WRITE;          // 生成時状態
    D3D12_RESOURCE_STATES m_pageTableState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // 生成時状態
public:
    // V4: VSM 描画パス群の前後で atlas/pageTable を working↔resting(SRV) へ遷移。
    void BeginRenderStates(ID3D12GraphicsCommandList* cmd);   // → working (DEPTH_WRITE / UAV)
    void EndRenderStates(ID3D12GraphicsCommandList* cmd);     // → PIXEL_SHADER_RESOURCE (resting)
private:

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
    ComPtr<ID3D12DescriptorHeap> m_allocHeap;          // [0]=Request SRV,[1]=PageTable UAV,[2]=PhysToVirtual UAV,[3]=Counter UAV,[4]=DirtyPageTable UAV
    UINT m_allocStride = 0;
    ComPtr<ID3D12RootSignature> m_allocRootSig;
    ComPtr<ID3D12PipelineState> m_allocPso;
    uint32_t m_allocFrame = 0;
    uint32_t m_lastAllocCount = 0;

    // V5b 永続キャッシュ
    bool m_cacheMode = false;         // DX12_VSM_CACHE / Scene::SetVsm... 経由
    bool m_cacheNeedsReset = true;    // 次回描画で PageTable/Counter/アトラスを初期化
    ComPtr<ID3D12Resource> m_dirtyPageTable;      // vp->phys（今フレーム新規のみ, 他0xFFFF）binning へ流す
    ComPtr<ID3D12Resource> m_physFrame;           // phys-> 最後に(再)割当されたVSM描画フレーム番号（同フレーム退去防止）
    uint32_t m_allocFrameCounter = 0;             // VSM 描画毎に++（gFrame としてAllocateへ）
    ComPtr<ID3D12Resource> m_residentAP;          // vp-> そのスロットが現在保持する絶対ページ(packed)。移動でスロットが
                                                  // 別世界ページに巻いた(wrap)ことを検出し再描画するためのキー。
    ComPtr<ID3D12Resource> m_pageTableInit;       // 0xFFFF 埋めアップロード（リセット時 PageTable へコピー）
    void ResetCacheGpu(ID3D12GraphicsCommandList* cmd);   // PageTable←0xFFFF, Counter←0, residentAP←0, アトラス全クリア

    // V3c-m1: per-page 描画パラメータ
    ComPtr<ID3D12Resource> m_pageCenterExtent;   // float4/page: cx,cy,extent,level
    ComPtr<ID3D12Resource> m_pageTile;           // uint4/page: px,py,tx,ty
    ComPtr<ID3D12DescriptorHeap> m_buildHeap;    // [0]=PhysToVirtual SRV,[1]=Counter SRV,[2]=PageCenterExtent UAV,[3]=PageTile UAV
    UINT m_buildStride = 0;
    ComPtr<ID3D12RootSignature> m_buildRootSig;
    ComPtr<ID3D12PipelineState> m_buildPso;

    // V3c-m2: キャスタ→ページ ビニング（すべて root descriptor, UNORDERED_ACCESS 常駐）
    D3D12_GPU_VIRTUAL_ADDRESS m_casterVA = 0;
    uint32_t m_casterCount = 0;
    uint32_t m_binModelCount = 0;
    ComPtr<ID3D12Resource> m_pairCount;      // uint × kMaxModels（モデル別ペア数）
    ComPtr<ID3D12Resource> m_pairBase;       // uint × kMaxModels（exclusive prefix）
    ComPtr<ID3D12Resource> m_pairCursor;     // uint × kMaxModels（scatter 書込カーソル）
    ComPtr<ID3D12Resource> m_instancePairs;  // uint2 × kMaxPairs（worldIdx, physPage）
    ComPtr<ID3D12Resource> m_globalCounter;  // uint（総試行ペア数=検証/監視）
    ComPtr<ID3D12Resource> m_binTotals;      // uint（総ペア数=Σ PairCount）
    ComPtr<ID3D12Resource> m_binReadback[kCbFrames];
    ComPtr<ID3D12RootSignature> m_binCountRS, m_binPrefixRS, m_binScatterRS;
    ComPtr<ID3D12PipelineState> m_binCountPso, m_binPrefixPso, m_binScatterPso;
    uint32_t m_binFrame = 0;
    uint32_t m_lastPairCount = 0;
    uint32_t m_lastPairAttempts = 0;

    // V3c-m2e/m3: 間接引数 + ページ描画
    static constexpr uint32_t kMaxBatches = 4096;
    D3D12_GPU_VIRTUAL_ADDRESS m_submeshTableVA = 0;
    uint32_t m_batchCount = 0;
    ComPtr<ID3D12Resource> m_drawArgs;              // DrawArgs(20B) × kMaxBatches
    ComPtr<ID3D12RootSignature> m_argsRS;
    ComPtr<ID3D12PipelineState> m_argsPso;
    ComPtr<ID3D12RootSignature> m_pageRenderRS;
    ComPtr<ID3D12PipelineState> m_pageRenderPso;
    ComPtr<ID3D12CommandSignature> m_pageCmdSig;

    // V5b: FIFO 再利用タイルの stale 除去（dirty タイルを深度1.0で事前クリア）
    ComPtr<ID3D12RootSignature> m_clearTilesRS;
    ComPtr<ID3D12PipelineState> m_clearTilesPso;

    // 検証: アトラス可視化
    ID3D12DescriptorHeap* m_sceneHeapRaw = nullptr;   // シーン記述子ヒープ（atlas SRV を含む）
    ComPtr<ID3D12RootSignature> m_atlasDebugRS;
    ComPtr<ID3D12PipelineState> m_atlasDebugPso;

    // 検証: 影サンプル（フルスクリーン）。専用ヒープ [0]=depth SRV,[1]=pageTable SRV,[2]=atlas SRV
    ComPtr<ID3D12DescriptorHeap> m_shadowDbgHeap;
    UINT m_shadowDbgStride = 0;
    bool m_shadowDbgSrvReady = false;
    ComPtr<ID3D12RootSignature> m_shadowDbgRS;
    ComPtr<ID3D12PipelineState> m_shadowDbgPso;
};
