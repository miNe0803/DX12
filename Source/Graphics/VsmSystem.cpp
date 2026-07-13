#include "VsmSystem.h"
#include "DescriptorHeap.h"
#include "Engine.h"
#include "DebugLog.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool VsmSystem::Init(ID3D12Device* device, DescriptorHeap* sceneHeap, uint32_t width, uint32_t height)
{
    if (!device || !sceneHeap) return false;
    m_w = width; m_h = height;
    m_sceneHeapRaw = sceneHeap->GetHeap();   // 検証用アトラス可視化のヒープ
    if (!CreateAtlas(device)) return false;
    if (!CreatePageTable(device, sceneHeap)) return false;
    if (!CreateConstantBuffer(device)) return false;
    if (!CreateRequestResources(device)) return false;
    if (!CreateMarkPipeline(device)) return false;
    if (!CreateAllocResources(device)) return false;
    if (!CreateAllocPipeline(device)) return false;
    if (!CreateBuildResources(device)) return false;
    if (!CreateBuildPipeline(device)) return false;
    if (!CreateBinningResources(device)) return false;
    if (!CreateBinningPipelines(device)) return false;
    if (!CreateDrawArgsPipeline(device)) return false;
    if (!CreatePageRenderPipeline(device)) return false;
    if (!CreateClearTilesPipeline(device)) return false;
    if (!CreateAtlasDebugPipeline(device)) return false;
    if (!CreateShadowDebugPipeline(device)) return false;

    // アトラス SRV（サンプル用, V4）をシーンヒープへ
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        DescriptorHandle* h = sceneHeap->RegisterResource(m_atlas.Get(), sd);
        if (h) m_atlasSrvGpu = h->HandleGPU;
    }

    m_valid = true;
    DebugLog("[VSM] Init OK: atlas %ux%u (%u pages), pageTable %u entries, %u levels, baseExtent %.1fm\n",
        kAtlasPagesPerRow * kPageSize, kAtlasPagesPerRow * kPageSize, kPhysicalPages,
        kTotalVirtualPages, kLevels, kBaseExtent);
    return true;
}

void VsmSystem::Shutdown()
{
    if (m_cb && m_cbMapped) { m_cb->Unmap(0, nullptr); m_cbMapped = nullptr; }
    m_valid = false;
}

bool VsmSystem::CreateAtlas(ID3D12Device* device)
{
    const UINT dim = kAtlasPagesPerRow * kPageSize;   // 8192
    auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, dim, dim, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_atlas))))
    { printf("VSM: atlas create failed\n"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_atlasDsvHeap)))) return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_atlas.Get(), &dsv,
        m_atlasDsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool VsmSystem::CreatePageTable(ID3D12Device* device, DescriptorHeap* sceneHeap)
{
    const UINT64 bytes = (UINT64)kTotalVirtualPages * sizeof(uint32_t);
    auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto rd = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_pageTable))))
    { printf("VSM: pageTable create failed\n"); return false; }

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_UNKNOWN; sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = kTotalVirtualPages; sd.Buffer.StructureByteStride = sizeof(uint32_t);
    DescriptorHandle* srv = sceneHeap->RegisterResource(m_pageTable.Get(), sd);
    if (srv) m_pageTableSrvGpu = srv->HandleGPU;

    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kTotalVirtualPages; ud.Buffer.StructureByteStride = sizeof(uint32_t);
    DescriptorHandle* uav = sceneHeap->CreateUAV(m_pageTable.Get(), ud);
    if (uav) m_pageTableUavGpu = uav->HandleGPU;
    return true;
}

bool VsmSystem::CreateConstantBuffer(ID3D12Device* device)
{
    auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)sizeof(VsmConstants) * kCbFrames);
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))))
    { printf("VSM: CB create failed\n"); return false; }
    if (FAILED(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped)))) return false;
    memset(m_cbMapped, 0, sizeof(VsmConstants) * kCbFrames);
    return true;
}

void VsmSystem::UpdateConstants(const XMMATRIX& lightView, const XMMATRIX& invViewProj,
    const XMFLOAT3& camPos, float lightZNear, float lightZFar)
{
    if (!m_valid) return;

    // V5a: カメラ(invViewProj)/太陽(lightView)が前回描画から変わったか＝再描画要否。
    // クリップマップはカメラ回転でも可視ページ集合が変わる（MarkPagesが深度依存）ため、
    // 平行移動・回転・太陽変化のいずれでも再描画が必要。静止フレームはスキップ。
    XMFLOAT4X4 lv, ivp;
    XMStoreFloat4x4(&lv, lightView);
    XMStoreFloat4x4(&ivp, invViewProj);
    m_needsRender = !m_hasLastView ||
        memcmp(&lv, &m_lastLightView, sizeof(lv)) != 0 ||
        memcmp(&ivp, &m_lastInvViewProj, sizeof(ivp)) != 0;
    if (m_needsRender)
    {
        m_lastLightView = lv;
        m_lastInvViewProj = ivp;
        m_hasLastView = true;
    }

    m_cbFrame = (m_cbFrame + 1) % kCbFrames;
    auto* c = reinterpret_cast<VsmConstants*>(m_cbMapped + (size_t)m_cbFrame * sizeof(VsmConstants));

    c->LightView = XMMatrixTranspose(lightView);      // HLSL column-major
    c->InvViewProj = XMMatrixTranspose(invViewProj);
    c->Params = XMFLOAT4((float)kLevels, (float)kPageSize, (float)kVirtualPagesPerRow, (float)kAtlasPagesPerRow);
    c->DepthDim = XMFLOAT4((float)m_w, (float)m_h, 1.0f / (float)m_w, 1.0f / (float)m_h);

    // カメラの光空間 XY（クリップマップ中心の基準）
    XMVECTOR camLS = XMVector3Transform(XMLoadFloat3(&camPos), lightView);
    float camLX = XMVectorGetX(camLS), camLY = XMVectorGetY(camLS);
    c->ZParams = XMFLOAT4(lightZNear, lightZFar, camLX, camLY);

    // V5b: 各レベル絶対トロイダル。LevelCenterExtent[i] の意味を変更 →
    // (originX, originY, pageWorld, texelWorld)。origin=窓原点=カメラ光空間を pageWorld 格子へ
    // 量子化し半幅(vppr/2)引いた「整数ページ座標」。可視点は [origin, origin+vppr) の窓内に入る。
    for (uint32_t i = 0; i < kLevels; ++i)
    {
        float extent = kBaseExtent * (float)(1u << i);
        float pageWorld = extent / (float)kVirtualPagesPerRow;   // 1ページの世界サイズ
        float originX = floorf(camLX / pageWorld) - (float)(kVirtualPagesPerRow / 2);
        float originY = floorf(camLY / pageWorld) - (float)(kVirtualPagesPerRow / 2);
        float texelWorld = pageWorld / (float)kPageSize;
        c->LevelCenterExtent[i] = XMFLOAT4(originX, originY, pageWorld, texelWorld);
    }
}

D3D12_GPU_VIRTUAL_ADDRESS VsmSystem::GetConstantsAddress() const
{
    return m_cb ? m_cb->GetGPUVirtualAddress() + (D3D12_GPU_VIRTUAL_ADDRESS)m_cbFrame * sizeof(VsmConstants) : 0;
}

bool VsmSystem::CreateRequestResources(ID3D12Device* device)
{
    const UINT64 bytes = (UINT64)kTotalVirtualPages * sizeof(uint32_t);
    auto def = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto rd = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_requestBuffer))))
    { printf("VSM: request buffer failed\n"); return false; }

    // クリア元（ゼロ）
    auto up = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto rb = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rb,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_zeroUpload))))
        return false;
    { void* p = nullptr; m_zeroUpload->Map(0, nullptr, &p); memset(p, 0, bytes); m_zeroUpload->Unmap(0, nullptr); }

    // 読戻し検証用
    auto rbp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    for (uint32_t i = 0; i < kCbFrames; ++i)
        if (FAILED(device->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rb,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_requestReadback[i]))))
            return false;

    // CS 用ヒープ [0]=depth SRV, [1]=request UAV
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_markHeap)))) return false;
    m_markStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool VsmSystem::CreateMarkPipeline(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER params[4] = {};
    params[0].InitAsConstantBufferView(0);
    CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    params[1].InitAsDescriptorTable(1, &srvR);
    CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    params[2].InitAsDescriptorTable(1, &uavR);
    params[3].InitAsConstants(1, 1);   // b1: gMaxMarkLevel（これより遠いレベルの画素はページ要求しない→CSM）
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 4; rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM mark RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_markRootSig))))
        return false;

    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DReadFileToBlob(L"VsmPageMark_CS.cso", &cs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmPageMark_CS.cso", &cs)))
    { printf("VSM: VsmPageMark_CS.cso not found\n"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_markRootSig.Get();
    pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_markPso))))
    { printf("VSM: mark PSO failed\n"); return false; }
    return true;
}

void VsmSystem::MarkPages(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depthResource)
{
    if (!m_valid || !depthResource) return;
    auto* dev = g_Engine->Device();
    const UINT64 bytes = (UINT64)kTotalVirtualPages * sizeof(uint32_t);
    GPU_CMD_BEGIN_EVENT(cmd, 160, 120, 220, L"VSM: page-mark");

    // 記述子: [0]=depth SRV, [1]=request UAV
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_markHeap->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_R32_FLOAT; dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; dd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(depthResource, &dd, base);
        D3D12_CPU_DESCRIPTOR_HANDLE h1 = base; h1.ptr += m_markStride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kTotalVirtualPages; ud.Buffer.StructureByteStride = sizeof(uint32_t);
        dev->CreateUnorderedAccessView(m_requestBuffer.Get(), nullptr, &ud, h1);
    }

    // (1) 要求バッファをクリア（ゼロコピー）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_requestBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_requestBuffer.Get(), 0, m_zeroUpload.Get(), 0, bytes);
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_requestBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }

    // (2) 深度 -> SRV, マーク CS
    auto dToSrv = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &dToSrv);

    ID3D12DescriptorHeap* heaps[] = { m_markHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_markRootSig.Get());
    cmd->SetPipelineState(m_markPso.Get());
    cmd->SetComputeRootConstantBufferView(0, GetConstantsAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE gbase = m_markHeap->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(1, gbase);
    D3D12_GPU_DESCRIPTOR_HANDLE gUav = gbase; gUav.ptr += m_markStride;
    cmd->SetComputeRootDescriptorTable(2, gUav);
    // VSMを近距離の連続レベルに限定（これより遠い=高レベルの画素はページ要求せず→町側でCSM）。
    // これでプール溢れ(密な遠景視界)による散在パッチ/ブロック破綻を防ぎ、近VSM＋遠CSMの綺麗な境界にする。
    // 既定 5（レベル0-5 ≒ 近~128m を VSM高精細、以遠は CSM）。密な街路の遠景がプールを溢れさせるのを防ぐ。
    // DX12_VSM_MAXLEVEL で調整（大=VSM遠くまで/溢れ risk, 小=近だけVSM・遠CSM）。
    static uint32_t s_maxMarkLevel = [] {
        char e[16]; return GetEnvironmentVariableA("DX12_VSM_MAXLEVEL", e, sizeof(e)) > 0 ? (uint32_t)atoi(e) : 5u;
    }();
    cmd->SetComputeRoot32BitConstants(3, 1, &s_maxMarkLevel, 0);
    cmd->Dispatch((m_w + 7) / 8, (m_h + 7) / 8, 1);

    auto dBack = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &dBack);

    // (3) 検証用に読戻しへコピー
    {
        auto uav = CD3DX12_RESOURCE_BARRIER::UAV(m_requestBuffer.Get());
        cmd->ResourceBarrier(1, &uav);
        auto toSrc = CD3DX12_RESOURCE_BARRIER::Transition(m_requestBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &toSrc);
        cmd->CopyBufferRegion(m_requestReadback[m_markFrame % kCbFrames].Get(), 0, m_requestBuffer.Get(), 0, bytes);
        auto back = CD3DX12_RESOURCE_BARRIER::Transition(m_requestBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &back);
    }

    // (4) 最古の読戻し（kCbFrames-1 フレーム前=GPU完了済）を集計（間引きログ）
    if ((++m_dbgThrottle % 120u) == 0u)
    {
        uint32_t oldest = (m_markFrame + 1u) % kCbFrames;
        void* p = nullptr; D3D12_RANGE rr{ 0, (SIZE_T)bytes };
        if (SUCCEEDED(m_requestReadback[oldest]->Map(0, &rr, &p)) && p)
        {
            const uint32_t* d = reinterpret_cast<const uint32_t*>(p);
            uint32_t cnt = 0; for (uint32_t i = 0; i < kTotalVirtualPages; ++i) if (d[i]) ++cnt;
            m_lastRequestCount = cnt;
            D3D12_RANGE wr{ 0, 0 }; m_requestReadback[oldest]->Unmap(0, &wr);
            printf("[VSM] requested pages this frame = %u / %u\n", cnt, kTotalVirtualPages);
            fflush(stdout);
        }
    }
    ++m_markFrame;
    GPU_CMD_END_EVENT(cmd);
}

bool VsmSystem::CreateAllocResources(ID3D12Device* device)
{
    auto def = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    // カウンタ（uint 1個）
    {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_counter)))) return false;
    }
    // 逆引き phys->vp
    {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)kPhysicalPages * sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_physToVirtual)))) return false;
    }
    // V5b 永続キャッシュ: dirty page table（今フレーム新規のみ, binning へ流す）
    {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)kTotalVirtualPages * sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_dirtyPageTable)))) return false;
    }
    // V5b: residentAP（vp -> 保持中の絶対ページ packed）。wrap 検出用。
    {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)kTotalVirtualPages * sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_residentAP)))) return false;
    }
    // V5b: physFrame（phys -> 最後に割当されたフレーム番号）。同フレーム退去防止用。
    {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)kPhysicalPages * sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_physFrame)))) return false;
    }
    // V5b: PageTable を 0xFFFF(=kInvalidPage) で初期化するアップロード元（リセット時にコピー）。
    {
        auto up = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)kTotalVirtualPages * sizeof(uint32_t));
        if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pageTableInit)))) return false;
        void* p = nullptr; D3D12_RANGE none{ 0, 0 };
        if (SUCCEEDED(m_pageTableInit->Map(0, &none, &p)) && p)
        {
            uint32_t* d = reinterpret_cast<uint32_t*>(p);
            for (uint32_t i = 0; i < kTotalVirtualPages; ++i) d[i] = kInvalidPage;   // 0xFFFF
            m_pageTableInit->Unmap(0, nullptr);
        }
    }
    // カウンタ読戻し
    auto rbp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto rbd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));
    for (uint32_t i = 0; i < kCbFrames; ++i)
        if (FAILED(device->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rbd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_counterReadback[i])))) return false;

    // ヒープ [0]=Request SRV,[1]=PageTable,[2]=PhysToVirtual,[3]=Counter,[4]=DirtyPageTable,[5]=residentAP,[6]=physFrame (UAV)
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 7;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_allocHeap)))) return false;
    m_allocStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_allocHeap->GetCPUDescriptorHandleForHeapStart();
    auto mkSrv = [&](ID3D12Resource* r, UINT n) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {}; s.Format = DXGI_FORMAT_UNKNOWN;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = n; s.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateShaderResourceView(r, &s, h); h.ptr += m_allocStride;
    };
    auto mkUav = [&](ID3D12Resource* r, UINT n) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {}; u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = n; u.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateUnorderedAccessView(r, nullptr, &u, h); h.ptr += m_allocStride;
    };
    mkSrv(m_requestBuffer.Get(), kTotalVirtualPages);
    mkUav(m_pageTable.Get(), kTotalVirtualPages);
    mkUav(m_physToVirtual.Get(), kPhysicalPages);
    mkUav(m_counter.Get(), 1);
    mkUav(m_dirtyPageTable.Get(), kTotalVirtualPages);   // u3 (cache)
    mkUav(m_residentAP.Get(), kTotalVirtualPages);       // u4 (cache: wrap 検出キー)
    mkUav(m_physFrame.Get(), kPhysicalPages);            // u5 (cache: 同フレーム退去防止)
    return true;
}

bool VsmSystem::CreateAllocPipeline(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER params[3] = {};
    params[0].InitAsConstants(4, 0);   // b0: gTotalVirtual, gPhysCap, pad, pad
    CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    params[1].InitAsDescriptorTable(1, &srvR);
    CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);  // u0..u5 (u3=dirty,u4=residentAP,u5=physFrame)
    params[2].InitAsDescriptorTable(1, &uavR);
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 3; rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM alloc RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_allocRootSig))))
        return false;

    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DReadFileToBlob(L"VsmAllocate_CS.cso", &cs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmAllocate_CS.cso", &cs)))
    { printf("VSM: VsmAllocate_CS.cso not found\n"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_allocRootSig.Get();
    pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_allocPso))))
    { printf("VSM: alloc PSO failed\n"); return false; }
    return true;
}

void VsmSystem::Allocate(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    GPU_CMD_BEGIN_EVENT(cmd, 200, 140, 100, L"VSM: page-alloc");

    // 永続キャッシュのリセット（有効化/太陽変化時に一度）: PageTable←0xFFFF, Counter←0, アトラス全クリア。
    // MarkPages 後・Allocate CS 前のこの位置なら atlas=DEPTH_WRITE, pageTable=UAV で安全。
    if (m_cacheMode && m_cacheNeedsReset)
    {
        ResetCacheGpu(cmd);
        m_cacheNeedsReset = false;
    }

    // カウンタを 0 クリア（ゼロバッファ先頭4Bをコピー）。
    // ※永続キャッシュ時はクリアしない（Counter は物理割当の高水位＝リセット時のみ 0 に戻す）。
    if (!m_cacheMode)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_counter.Get(), 0, m_zeroUpload.Get(), 0, sizeof(uint32_t));
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    // Request を SRV 状態へ（マーク後は UNORDERED_ACCESS）
    auto rToSrv = CD3DX12_RESOURCE_BARRIER::Transition(m_requestBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &rToSrv);

    ID3D12DescriptorHeap* heaps[] = { m_allocHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_allocRootSig.Get());
    cmd->SetPipelineState(m_allocPso.Get());
    // 診断: DX12_VSM_POOLCAP で物理プールの実効サイズを縮小し、通常視点でも枯渇(intra-frame FIFO wrap)を
    // 強制再現できるようにする（ユーザーの密な街路視点の破綻を切り分けるため）。0=無効(=4096)。
    static uint32_t s_poolCap = [] { char e[16]; return GetEnvironmentVariableA("DX12_VSM_POOLCAP", e, sizeof(e)) > 0 ? (uint32_t)atoi(e) : 0u; }();
    uint32_t effCap = (s_poolCap > 0u && s_poolCap < kPhysicalPages) ? s_poolCap : kPhysicalPages;
    uint32_t consts[4] = { kTotalVirtualPages, effCap, m_cacheMode ? 1u : 0u, ++m_allocFrameCounter };
    cmd->SetComputeRoot32BitConstants(0, 4, consts, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gbase = m_allocHeap->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(1, gbase);                          // t0 Request SRV
    D3D12_GPU_DESCRIPTOR_HANDLE gUav = gbase; gUav.ptr += m_allocStride;   // u0..u2 = [1,2,3]
    cmd->SetComputeRootDescriptorTable(2, gUav);
    cmd->Dispatch((kTotalVirtualPages + 63) / 64, 1, 1);

    // Request を UNORDERED_ACCESS へ戻す（次フレームのマーククリア用）
    auto rBack = CD3DX12_RESOURCE_BARRIER::Transition(m_requestBuffer.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &rBack);

    // 検証: カウンタ読戻し（間引きログ）
    {
        auto uav = CD3DX12_RESOURCE_BARRIER::UAV(m_counter.Get()); cmd->ResourceBarrier(1, &uav);
        auto toSrc = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &toSrc);
        cmd->CopyBufferRegion(m_counterReadback[m_allocFrame % kCbFrames].Get(), 0, m_counter.Get(), 0, sizeof(uint32_t));
        auto back = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &back);
    }
    if ((m_dbgThrottle % 120u) == 1u)   // MarkPages と別位相でログ
    {
        uint32_t oldest = (m_allocFrame + 1u) % kCbFrames;
        void* p = nullptr; D3D12_RANGE rr{ 0, sizeof(uint32_t) };
        if (SUCCEEDED(m_counterReadback[oldest]->Map(0, &rr, &p)) && p)
        {
            m_lastAllocCount = *reinterpret_cast<const uint32_t*>(p);
            D3D12_RANGE wr{ 0, 0 }; m_counterReadback[oldest]->Unmap(0, &wr);
            printf("[VSM] allocated pages = %u (cap %u)\n", m_lastAllocCount, kPhysicalPages);
            fflush(stdout);
        }
    }
    ++m_allocFrame;
    GPU_CMD_END_EVENT(cmd);
}

bool VsmSystem::CreateBuildResources(ID3D12Device* device)
{
    auto def = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    // per-page パラメータ（float4 と uint4）
    auto mkBuf = [&](UINT64 elemSize, ComPtr<ID3D12Resource>& out) {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(elemSize * kPhysicalPages, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!mkBuf(sizeof(float) * 4, m_pageCenterExtent)) return false;
    if (!mkBuf(sizeof(uint32_t) * 4, m_pageTile)) return false;

    // ヒープ [0]=PhysToVirtual SRV,[1]=Counter SRV,[2]=PageCenterExtent UAV,[3]=PageTile UAV
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_buildHeap)))) return false;
    m_buildStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_buildHeap->GetCPUDescriptorHandleForHeapStart();
    auto mkSrv = [&](ID3D12Resource* r, UINT n, UINT stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {}; s.Format = DXGI_FORMAT_UNKNOWN;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = n; s.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(r, &s, h); h.ptr += m_buildStride;
    };
    auto mkUav = [&](ID3D12Resource* r, UINT n, UINT stride) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {}; u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = n; u.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(r, nullptr, &u, h); h.ptr += m_buildStride;
    };
    mkSrv(m_physToVirtual.Get(), kPhysicalPages, sizeof(uint32_t));
    mkSrv(m_counter.Get(), 1, sizeof(uint32_t));
    mkUav(m_pageCenterExtent.Get(), kPhysicalPages, sizeof(float) * 4);
    mkUav(m_pageTile.Get(), kPhysicalPages, sizeof(uint32_t) * 4);
    return true;
}

bool VsmSystem::CreateBuildPipeline(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER params[3] = {};
    params[0].InitAsConstantBufferView(0);              // b0 = VsmConstants
    CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);   // t0,t1
    params[1].InitAsDescriptorTable(1, &srvR);
    CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);   // u0,u1
    params[2].InitAsDescriptorTable(1, &uavR);
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 3; rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM build RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_buildRootSig))))
        return false;

    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DReadFileToBlob(L"VsmBuildPageParams_CS.cso", &cs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmBuildPageParams_CS.cso", &cs)))
    { printf("VSM: VsmBuildPageParams_CS.cso not found\n"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_buildRootSig.Get();
    pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_buildPso))))
    { printf("VSM: build PSO failed\n"); return false; }
    return true;
}

void VsmSystem::BuildPageParams(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    GPU_CMD_BEGIN_EVENT(cmd, 100, 180, 200, L"VSM: build page params");

    // PhysToVirtual/Counter を SRV 状態へ（Allocate 後は UAV）
    D3D12_RESOURCE_BARRIER pre[2];
    pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_physToVirtual.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(2, pre);

    ID3D12DescriptorHeap* heaps[] = { m_buildHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_buildRootSig.Get());
    cmd->SetPipelineState(m_buildPso.Get());
    cmd->SetComputeRootConstantBufferView(0, GetConstantsAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE gbase = m_buildHeap->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(1, gbase);                            // t0,t1
    D3D12_GPU_DESCRIPTOR_HANDLE gUav = gbase; gUav.ptr += 2 * m_buildStride; // u0,u1
    cmd->SetComputeRootDescriptorTable(2, gUav);
    cmd->Dispatch((kPhysicalPages + 63) / 64, 1, 1);

    D3D12_RESOURCE_BARRIER post[2];
    post[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_physToVirtual.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    post[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(2, post);
    GPU_CMD_END_EVENT(cmd);
}

// ============================================================
//  V3c-m2: キャスタ→ページ ビニング
// ============================================================
void VsmSystem::SetCasterSource(D3D12_GPU_VIRTUAL_ADDRESS casterVA, uint32_t casterCount, uint32_t modelCount,
                                D3D12_GPU_VIRTUAL_ADDRESS submeshTableVA, uint32_t batchCount)
{
    m_casterVA = casterVA;
    m_casterCount = casterCount;
    m_binModelCount = (modelCount <= kMaxModels) ? modelCount : kMaxModels;
    m_submeshTableVA = submeshTableVA;
    m_batchCount = (batchCount <= kMaxBatches) ? batchCount : kMaxBatches;
    if (modelCount > kMaxModels)
        printf("[VSM] WARNING: modelCount %u > kMaxModels %u (clamped)\n", modelCount, kMaxModels);
    if (batchCount > kMaxBatches)
        printf("[VSM] WARNING: batchCount %u > kMaxBatches %u (clamped)\n", batchCount, kMaxBatches);
    printf("[VSM] caster source set: %u casters, %u models, %u batches\n", m_casterCount, m_binModelCount, m_batchCount);
    fflush(stdout);
}

bool VsmSystem::CreateBinningResources(ID3D12Device* device)
{
    auto def = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto mk = [&](UINT64 bytes, ComPtr<ID3D12Resource>& out) {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!mk((UINT64)kMaxModels * sizeof(uint32_t), m_pairCount)) return false;
    if (!mk((UINT64)kMaxModels * sizeof(uint32_t), m_pairBase)) return false;
    if (!mk((UINT64)kMaxModels * sizeof(uint32_t), m_pairCursor)) return false;
    if (!mk((UINT64)kMaxPairs * 2 * sizeof(uint32_t), m_instancePairs)) return false; // uint2
    if (!mk(sizeof(uint32_t), m_globalCounter)) return false;
    if (!mk(sizeof(uint32_t), m_binTotals)) return false;

    auto rbp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto rbd = CD3DX12_RESOURCE_DESC::Buffer(2 * sizeof(uint32_t)); // [0]=total, [1]=attempts
    for (uint32_t i = 0; i < kCbFrames; ++i)
        if (FAILED(device->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rbd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_binReadback[i])))) return false;
    return true;
}

bool VsmSystem::CreateBinningPipelines(ID3D12Device* device)
{
    auto makePso = [&](const wchar_t* cso, const wchar_t* csoAlt,
                       ID3D12RootSignature* rs, ComPtr<ID3D12PipelineState>& pso) -> bool {
        ComPtr<ID3DBlob> b;
        if (FAILED(D3DReadFileToBlob(cso, &b)) && FAILED(D3DReadFileToBlob(csoAlt, &b)))
        { wprintf(L"VSM: %s not found\n", cso); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.CS = CD3DX12_SHADER_BYTECODE(b.Get());
        return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    };
    auto serialize = [&](const D3D12_ROOT_SIGNATURE_DESC& rs, ComPtr<ID3D12RootSignature>& out) -> bool {
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("VSM bin RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        return SUCCEEDED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&out)));
    };

    // count: b0 VsmCB, b1 consts(4), t0 Casters, t1 PageTable, u0 PairCount
    {
        CD3DX12_ROOT_PARAMETER p[5];
        p[0].InitAsConstantBufferView(0);
        p[1].InitAsConstants(4, 1);
        p[2].InitAsShaderResourceView(0);
        p[3].InitAsShaderResourceView(1);
        p[4].InitAsUnorderedAccessView(0);
        D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 5; rs.pParameters = p;
        if (!serialize(rs, m_binCountRS)) return false;
        if (!makePso(L"VsmCasterCount_CS.cso", L"Shaders\\Vsm\\VsmCasterCount_CS.cso", m_binCountRS.Get(), m_binCountPso)) return false;
    }
    // prefix: b0 consts(4), u0 PairCount, u1 PairBase, u2 PairCursor, u3 Totals
    {
        CD3DX12_ROOT_PARAMETER p[5];
        p[0].InitAsConstants(4, 0);
        p[1].InitAsUnorderedAccessView(0);
        p[2].InitAsUnorderedAccessView(1);
        p[3].InitAsUnorderedAccessView(2);
        p[4].InitAsUnorderedAccessView(3);
        D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 5; rs.pParameters = p;
        if (!serialize(rs, m_binPrefixRS)) return false;
        if (!makePso(L"VsmPrefixSum_CS.cso", L"Shaders\\Vsm\\VsmPrefixSum_CS.cso", m_binPrefixRS.Get(), m_binPrefixPso)) return false;
    }
    // scatter: b0 VsmCB, b1 consts(4), t0 Casters, t1 PageTable, u0 PairBase, u1 PairCursor, u2 PairCount, u3 InstancePairs, u4 GlobalCounter
    {
        CD3DX12_ROOT_PARAMETER p[9];
        p[0].InitAsConstantBufferView(0);
        p[1].InitAsConstants(4, 1);
        p[2].InitAsShaderResourceView(0);
        p[3].InitAsShaderResourceView(1);
        p[4].InitAsUnorderedAccessView(0);
        p[5].InitAsUnorderedAccessView(1);
        p[6].InitAsUnorderedAccessView(2);
        p[7].InitAsUnorderedAccessView(3);
        p[8].InitAsUnorderedAccessView(4);
        D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 9; rs.pParameters = p;
        if (!serialize(rs, m_binScatterRS)) return false;
        if (!makePso(L"VsmCasterScatter_CS.cso", L"Shaders\\Vsm\\VsmCasterScatter_CS.cso", m_binScatterRS.Get(), m_binScatterPso)) return false;
    }
    return true;
}

void VsmSystem::ZeroBuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, uint64_t bytes)
{
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(res,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &b);
    cmd->CopyBufferRegion(res, 0, m_zeroUpload.Get(), 0, bytes);
    b = CD3DX12_RESOURCE_BARRIER::Transition(res,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &b);
}

void VsmSystem::BuildCasterBinning(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid || !m_casterVA || m_casterCount == 0 || m_binModelCount == 0) return;
    GPU_CMD_BEGIN_EVENT(cmd, 220, 180, 80, L"VSM: caster binning");

    // binning が root SRV で読むページテーブル（cache=dirty, 非cache=全常駐）を SRV へ（Allocate 後は UAV）。
    ID3D12Resource* binTable = (m_cacheMode ? m_dirtyPageTable : m_pageTable).Get();
    auto ptToSrv = CD3DX12_RESOURCE_BARRIER::Transition(binTable,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &ptToSrv);

    // per-model カウンタ + グローバルカウンタを 0 クリア（PairBase/PairCursor は prefix-sum が全上書き）
    ZeroBuffer(cmd, m_pairCount.Get(), (UINT64)m_binModelCount * sizeof(uint32_t));
    ZeroBuffer(cmd, m_globalCounter.Get(), sizeof(uint32_t));

    const uint32_t consts[4] = { m_casterCount, m_binModelCount, kPhysicalPages, kMaxPairs };
    // 永続キャッシュ時は「今フレーム新規ページのみ」の dirty table を binning へ流す＝新規ページだけ描画。
    // 非キャッシュ時は従来通り全常駐ページ(=毎フレーム再割当)を流す。（count/scatter シェーダは無改変）
    const D3D12_GPU_VIRTUAL_ADDRESS pageTableVA =
        (m_cacheMode ? m_dirtyPageTable : m_pageTable)->GetGPUVirtualAddress();

    // --- m2b: count ---
    cmd->SetComputeRootSignature(m_binCountRS.Get());
    cmd->SetPipelineState(m_binCountPso.Get());
    cmd->SetComputeRootConstantBufferView(0, GetConstantsAddress());
    cmd->SetComputeRoot32BitConstants(1, 4, consts, 0);
    cmd->SetComputeRootShaderResourceView(2, m_casterVA);
    cmd->SetComputeRootShaderResourceView(3, pageTableVA);
    cmd->SetComputeRootUnorderedAccessView(4, m_pairCount->GetGPUVirtualAddress());
    cmd->Dispatch((m_casterCount + 63) / 64, 1, 1);
    { auto u = CD3DX12_RESOURCE_BARRIER::UAV(m_pairCount.Get()); cmd->ResourceBarrier(1, &u); }

    // --- m2c: prefix-sum ---
    cmd->SetComputeRootSignature(m_binPrefixRS.Get());
    cmd->SetPipelineState(m_binPrefixPso.Get());
    cmd->SetComputeRoot32BitConstants(0, 4, consts, 0);
    cmd->SetComputeRootUnorderedAccessView(1, m_pairCount->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, m_pairBase->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_pairCursor->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, m_binTotals->GetGPUVirtualAddress());
    cmd->Dispatch(1, 1, 1);
    {
        D3D12_RESOURCE_BARRIER u[3] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_pairBase.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_pairCursor.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_binTotals.Get()) };
        cmd->ResourceBarrier(3, u);
    }

    // --- m2d: scatter ---
    cmd->SetComputeRootSignature(m_binScatterRS.Get());
    cmd->SetPipelineState(m_binScatterPso.Get());
    cmd->SetComputeRootConstantBufferView(0, GetConstantsAddress());
    cmd->SetComputeRoot32BitConstants(1, 4, consts, 0);
    cmd->SetComputeRootShaderResourceView(2, m_casterVA);
    cmd->SetComputeRootShaderResourceView(3, pageTableVA);
    cmd->SetComputeRootUnorderedAccessView(4, m_pairBase->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(5, m_pairCursor->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, m_pairCount->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(7, m_instancePairs->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, m_globalCounter->GetGPUVirtualAddress());
    cmd->Dispatch((m_casterCount + 63) / 64, 1, 1);
    { auto u = CD3DX12_RESOURCE_BARRIER::UAV(m_instancePairs.Get()); cmd->ResourceBarrier(1, &u); }

    // --- m2e: 間接引数生成（PairBase/PairCount → DrawArgs、submesh バッチ毎）---
    if (m_submeshTableVA && m_batchCount > 0)
    {
        const uint32_t argConsts[4] = { m_batchCount, kMaxPairs, 0, 0 };
        cmd->SetComputeRootSignature(m_argsRS.Get());
        cmd->SetPipelineState(m_argsPso.Get());
        cmd->SetComputeRoot32BitConstants(0, 4, argConsts, 0);
        cmd->SetComputeRootShaderResourceView(1, m_submeshTableVA);
        cmd->SetComputeRootUnorderedAccessView(2, m_pairBase->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3, m_pairCount->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(4, m_drawArgs->GetGPUVirtualAddress());
        cmd->Dispatch((m_batchCount + 63) / 64, 1, 1);
        auto u = CD3DX12_RESOURCE_BARRIER::UAV(m_drawArgs.Get()); cmd->ResourceBarrier(1, &u);
    }

    // binTable を UAV へ復帰（C3: 次フレーム Allocate 用）
    auto ptBack = CD3DX12_RESOURCE_BARRIER::Transition(binTable,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &ptBack);

    // --- 検証: Totals + GlobalCounter を読戻し ---
    {
        auto s0 = CD3DX12_RESOURCE_BARRIER::Transition(m_binTotals.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto s1 = CD3DX12_RESOURCE_BARRIER::Transition(m_globalCounter.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_BARRIER pre[2] = { s0, s1 }; cmd->ResourceBarrier(2, pre);
        ID3D12Resource* rb = m_binReadback[m_binFrame % kCbFrames].Get();
        cmd->CopyBufferRegion(rb, 0, m_binTotals.Get(), 0, sizeof(uint32_t));
        cmd->CopyBufferRegion(rb, sizeof(uint32_t), m_globalCounter.Get(), 0, sizeof(uint32_t));
        auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(m_binTotals.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_globalCounter.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER post[2] = { b0, b1 }; cmd->ResourceBarrier(2, post);
    }
    if ((m_dbgThrottle % 120u) == 2u)   // mark=0 / alloc=1 と別位相
    {
        uint32_t oldest = (m_binFrame + 1u) % kCbFrames;
        void* p = nullptr; D3D12_RANGE rr{ 0, 2 * sizeof(uint32_t) };
        if (SUCCEEDED(m_binReadback[oldest]->Map(0, &rr, &p)) && p)
        {
            const uint32_t* d = reinterpret_cast<const uint32_t*>(p);
            m_lastPairCount = d[0]; m_lastPairAttempts = d[1];
            D3D12_RANGE wr{ 0, 0 }; m_binReadback[oldest]->Unmap(0, &wr);
            printf("[VSM] binning: total pairs=%u, attempts=%u, cap=%u%s\n",
                m_lastPairCount, m_lastPairAttempts, kMaxPairs,
                (m_lastPairCount != m_lastPairAttempts) ? " [MISMATCH!]" :
                (m_lastPairCount >= kMaxPairs) ? " [CAP HIT!]" : " OK");
            fflush(stdout);
        }
    }
    ++m_binFrame;
    GPU_CMD_END_EVENT(cmd);
}

// ============================================================
//  V3c-m2e/m3: 間接引数 + ページ描画
// ============================================================
bool VsmSystem::CreateDrawArgsPipeline(ID3D12Device* device)
{
    auto def = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)kMaxBatches * 5 * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_drawArgs)))) return false;

    CD3DX12_ROOT_PARAMETER p[5];
    p[0].InitAsConstants(4, 0);
    p[1].InitAsShaderResourceView(0);   // t0 SubmeshGeoTable
    p[2].InitAsUnorderedAccessView(0);  // u0 PairBase
    p[3].InitAsUnorderedAccessView(1);  // u1 PairCount
    p[4].InitAsUnorderedAccessView(2);  // u2 DrawArgsOut
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 5; rs.pParameters = p;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM args RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_argsRS)))) return false;

    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DReadFileToBlob(L"VsmBuildDrawArgs_CS.cso", &cs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmBuildDrawArgs_CS.cso", &cs)))
    { printf("VSM: VsmBuildDrawArgs_CS.cso not found\n"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_argsRS.Get(); pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_argsPso)))) { printf("VSM: args PSO failed\n"); return false; }
    return true;
}

bool VsmSystem::CreatePageRenderPipeline(ID3D12Device* device)
{
    // root sig: b0 VsmCB, b1 consts(4), t0 Casters, t1 PageCenterExtent, t2 PageTile
    CD3DX12_ROOT_PARAMETER p[5];
    p[0].InitAsConstantBufferView(0);
    p[1].InitAsConstants(4, 1);
    p[2].InitAsShaderResourceView(0);
    p[3].InitAsShaderResourceView(1);
    p[4].InitAsShaderResourceView(2);
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 5; rs.pParameters = p;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM pageRender RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_pageRenderRS)))) return false;

    ComPtr<ID3DBlob> vs;
    if (FAILED(D3DReadFileToBlob(L"VsmCasterDepth_VS.cso", &vs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmCasterDepth_VS.cso", &vs)))
    { printf("VSM: VsmCasterDepth_VS.cso not found\n"); return false; }

    static const D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_UINT,     1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_pageRenderRS.Get();
    pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    pd.InputLayout = { il, _countof(il) };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthBias = 2500;
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pd.RasterizerState.DepthClipEnable = TRUE;     // H2: 範囲外 Z を HW クリップ
    pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // depth write ALL, LESS
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pageRenderPso))))
    { printf("VSM: pageRender PSO failed\n"); return false; }

    D3D12_INDIRECT_ARGUMENT_DESC arg = {}; arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC csd = {};
    csd.ByteStride = 5 * sizeof(uint32_t); csd.NumArgumentDescs = 1; csd.pArgumentDescs = &arg;
    if (FAILED(device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&m_pageCmdSig))))
    { printf("VSM: page cmd sig failed\n"); return false; }
    return true;
}

// V5b: dirty タイルを深度1.0でクリアする軽量パイプライン（頂点はSV_VertexID生成, PSなし, DepthFunc ALWAYS）
bool VsmSystem::CreateClearTilesPipeline(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER p[2];
    p[0].InitAsConstants(4, 0);              // b0: appr, pageSize, atlasDim, totalVp
    p[1].InitAsUnorderedAccessView(0);       // u0: DirtyPageTable（VSでUAV読取）
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 2; rs.pParameters = p;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM clearTiles RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_clearTilesRS)))) return false;

    ComPtr<ID3DBlob> vs;
    if (FAILED(D3DReadFileToBlob(L"VsmClearTiles_VS.cso", &vs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmClearTiles_VS.cso", &vs)))
    { printf("VSM: VsmClearTiles_VS.cso not found\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_clearTilesRS.Get();
    pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    pd.InputLayout = { nullptr, 0 };                                  // 頂点は SV_VertexID から生成
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = FALSE;                       // z=1.0 を確実に通す
    pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;    // 無条件上書き＝クリア
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_clearTilesPso))))
    { printf("VSM: clearTiles PSO failed\n"); return false; }
    return true;
}

void VsmSystem::RenderPages(ID3D12GraphicsCommandList* cmd, const RenderBatch* batches, uint32_t count)
{
    if (!m_valid || count == 0 || m_batchCount == 0 || !m_submeshTableVA || !m_casterVA) return;
    GPU_CMD_BEGIN_EVENT(cmd, 120, 200, 120, L"VSM: page render");
    m_renderedCBAddr = GetConstantsAddress();   // V5b Stage0: このアトラスを描いた中心。町サンプルが後で参照

    // 遷移: params→SRV(VS読), InstancePairs→VB, DrawArgs→INDIRECT
    D3D12_RESOURCE_BARRIER pre[4];
    pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_pageCenterExtent.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_pageTile.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[2] = CD3DX12_RESOURCE_BARRIER::Transition(m_instancePairs.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    pre[3] = CD3DX12_RESOURCE_BARRIER::Transition(m_drawArgs.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cmd->ResourceBarrier(4, pre);

    // アトラス（常時 DEPTH_WRITE）へ深度描画
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_atlasDsvHeap->GetCPUDescriptorHandleForHeapStart();
    // 非キャッシュ: 毎フレーム全クリア（全ページ再描画）。
    // 永続キャッシュ: クリアしない（新規physのタイルはリセット時の全クリア以降未使用＝pristine=1.0、
    //   既存ページはキャッシュ内容を保持）。全クリアはリセット時 ResetCacheGpu が一度だけ行う。
    if (!m_cacheMode)
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    const UINT dim = kAtlasPagesPerRow * kPageSize;   // 8192
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)dim, (float)dim, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)dim, (LONG)dim };
    cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &sc);

    // 永続キャッシュ: dirty（今フレーム(再)割当）タイルだけを深度1.0で事前クリア（FIFO再利用タイルの
    // stale 除去）。DirtyPageTable は UAV 状態のまま VS で読む。この後キャスタが LESS で最近深度を書く。
    if (m_cacheMode)
    {
        cmd->SetPipelineState(m_clearTilesPso.Get());
        cmd->SetGraphicsRootSignature(m_clearTilesRS.Get());
        const uint32_t cc[4] = { kAtlasPagesPerRow, kPageSize, kAtlasPagesPerRow * kPageSize, kTotalVirtualPages };
        cmd->SetGraphicsRoot32BitConstants(0, 4, cc, 0);
        cmd->SetGraphicsRootUnorderedAccessView(1, m_dirtyPageTable->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmd->DrawInstanced(4, kTotalVirtualPages, 0, 0);
    }

    cmd->SetPipelineState(m_pageRenderPso.Get());
    cmd->SetGraphicsRootSignature(m_pageRenderRS.Get());
    cmd->SetGraphicsRootConstantBufferView(0, GetConstantsAddress());
    const uint32_t consts[4] = { m_casterCount, kPhysicalPages, 0, 0 };
    cmd->SetGraphicsRoot32BitConstants(1, 4, consts, 0);
    cmd->SetGraphicsRootShaderResourceView(2, m_casterVA);
    cmd->SetGraphicsRootShaderResourceView(3, m_pageCenterExtent->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(4, m_pageTile->GetGPUVirtualAddress());

    D3D12_VERTEX_BUFFER_VIEW instVBV = {
        m_instancePairs->GetGPUVirtualAddress(),
        (UINT)(kMaxPairs * 2 * sizeof(uint32_t)),
        (UINT)(2 * sizeof(uint32_t)) };
    cmd->IASetVertexBuffers(1, 1, &instVBV);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    uint32_t n = (count <= m_batchCount) ? count : m_batchCount;
    for (uint32_t b = 0; b < n; ++b)
    {
        cmd->IASetVertexBuffers(0, 1, &batches[b].vbv);
        cmd->IASetIndexBuffer(&batches[b].ibv);
        cmd->ExecuteIndirect(m_pageCmdSig.Get(), 1, m_drawArgs.Get(),
            (UINT64)b * 5 * sizeof(uint32_t), nullptr, 0);
    }

    // 遷移復帰（C3: 次フレームの BuildPageParams/scatter/m2e が UAV 前提）
    D3D12_RESOURCE_BARRIER post[4];
    post[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_pageCenterExtent.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    post[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_pageTile.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    post[2] = CD3DX12_RESOURCE_BARRIER::Transition(m_instancePairs.Get(),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    post[3] = CD3DX12_RESOURCE_BARRIER::Transition(m_drawArgs.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(4, post);
    GPU_CMD_END_EVENT(cmd);
}

void VsmSystem::SetCacheMode(bool on)
{
    if (on == m_cacheMode) return;
    m_cacheMode = on;
    m_cacheNeedsReset = true;   // モード切替時は次回描画で初期化（PageTable/Counter/アトラス）
}

// 永続キャッシュのリセット: PageTable←0xFFFF, Counter←0, アトラス全クリア。
// BeginRenderStates 後（atlas=DEPTH_WRITE, pageTable=UAV）に MarkPages より前で一度だけ呼ぶ。
void VsmSystem::ResetCacheGpu(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    GPU_CMD_BEGIN_EVENT(cmd, 220, 120, 120, L"VSM: cache reset");
    const UINT64 ptBytes = (UINT64)kTotalVirtualPages * sizeof(uint32_t);

    // PageTable ← 0xFFFF（UAV→COPY_DEST→UAV）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_pageTable.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_pageTable.Get(), 0, m_pageTableInit.Get(), 0, ptBytes);
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_pageTable.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    // Counter ← 0（UAV→COPY_DEST→UAV）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_counter.Get(), 0, m_zeroUpload.Get(), 0, sizeof(uint32_t));
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    // residentAP ← 0（全スロット「未保持」。次フレーム全要求が mismatch→再割当）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_residentAP.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_residentAP.Get(), 0, m_zeroUpload.Get(), 0, ptBytes);
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_residentAP.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    // PhysToVirtual ← 0xFFFF（>kTotalVirtualPages なので「旧所有者なし」の番兵。リング初回サイクルで誤退去を防ぐ）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_physToVirtual.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_physToVirtual.Get(), 0, m_pageTableInit.Get(), 0, (UINT64)kPhysicalPages * sizeof(uint32_t));
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_physToVirtual.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    // PhysFrame ← 0（フレーム番号は1から振るので 0 は「未使用」を意味＝初回サイクルで退去可能）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_physFrame.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_physFrame.Get(), 0, m_zeroUpload.Get(), 0, (UINT64)kPhysicalPages * sizeof(uint32_t));
        b = CD3DX12_RESOURCE_BARRIER::Transition(m_physFrame.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    // アトラス全クリア（1.0）。以降キャッシュ時は RenderPages でクリアしない。
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_atlasDsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    GPU_CMD_END_EVENT(cmd);
}

void VsmSystem::BeginRenderStates(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    D3D12_RESOURCE_BARRIER b[2]; UINT n = 0;
    if (m_atlasState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_atlas.Get(), m_atlasState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_atlasState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (m_pageTableState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_pageTable.Get(), m_pageTableState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_pageTableState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (n) cmd->ResourceBarrier(n, b);
}

void VsmSystem::EndRenderStates(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    D3D12_RESOURCE_BARRIER b[2]; UINT n = 0;
    if (m_atlasState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_atlas.Get(), m_atlasState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_atlasState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (m_pageTableState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_pageTable.Get(), m_pageTableState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_pageTableState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (n) cmd->ResourceBarrier(n, b);
}

bool VsmSystem::CreateAtlasDebugPipeline(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE srv; srv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);   // t0 atlas
    CD3DX12_ROOT_PARAMETER p[1];
    p[0].InitAsDescriptorTable(1, &srv, D3D12_SHADER_VISIBILITY_PIXEL);
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ShaderRegister = 0; smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1; rs.pParameters = p; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &smp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM atlasDebug RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_atlasDebugRS)))) return false;

    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DReadFileToBlob(L"ToneMap_VS.cso", &vs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", &vs)))
    { printf("VSM: ToneMap_VS.cso not found (atlasDebug)\n"); return false; }
    if (FAILED(D3DReadFileToBlob(L"VsmAtlasDebug_PS.cso", &ps)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmAtlasDebug_PS.cso", &ps)))
    { printf("VSM: VsmAtlasDebug_PS.cso not found\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_atlasDebugRS.Get();
    pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    pd.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_atlasDebugPso))))
    { printf("VSM: atlasDebug PSO failed\n"); return false; }
    return true;
}

void VsmSystem::RenderAtlasDebug(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv)
{
    if (!m_valid || !m_atlasDebugPso || !m_sceneHeapRaw) return;
    GPU_CMD_BEGIN_EVENT(cmd, 240, 240, 120, L"VSM: atlas debug view");

    if (m_atlasState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(m_atlas.Get(), m_atlasState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &toSrv);
        m_atlasState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)m_w, (float)m_h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)m_w, (LONG)m_h };
    cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = { m_sceneHeapRaw };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetPipelineState(m_atlasDebugPso.Get());
    cmd->SetGraphicsRootSignature(m_atlasDebugRS.Get());
    cmd->SetGraphicsRootDescriptorTable(0, m_atlasSrvGpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    // atlas は PIXEL_SHADER_RESOURCE(resting) のまま（町がサンプルする）
    GPU_CMD_END_EVENT(cmd);
}

bool VsmSystem::CreateShadowDebugPipeline(ID3D12Device* device)
{
    // 専用ヒープ [0]=depth SRV(遅延), [1]=pageTable SRV, [2]=atlas SRV
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_shadowDbgHeap)))) return false;
    m_shadowDbgStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_shadowDbgHeap->GetCPUDescriptorHandleForHeapStart();
    // [1] pageTable SRV
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h1 = h; h1.ptr += (SIZE_T)m_shadowDbgStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {}; s.Format = DXGI_FORMAT_UNKNOWN;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = kTotalVirtualPages; s.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateShaderResourceView(m_pageTable.Get(), &s, h1);
    }
    // [2] atlas SRV
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = h; h2.ptr += (SIZE_T)m_shadowDbgStride * 2;
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {}; s.Format = DXGI_FORMAT_R32_FLOAT;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_atlas.Get(), &s, h2);
    }

    CD3DX12_DESCRIPTOR_RANGE srv; srv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);  // t0,t1,t2
    CD3DX12_ROOT_PARAMETER p[2];
    p[0].InitAsConstantBufferView(0);                                   // b0 VsmCB
    p[1].InitAsDescriptorTable(1, &srv, D3D12_SHADER_VISIBILITY_PIXEL);
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ShaderRegister = 0; smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2; rs.pParameters = p; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &smp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { if (err) printf("VSM shadowDbg RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_shadowDbgRS)))) return false;

    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DReadFileToBlob(L"ToneMap_VS.cso", &vs)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", &vs)))
    { printf("VSM: ToneMap_VS.cso not found (shadowDbg)\n"); return false; }
    if (FAILED(D3DReadFileToBlob(L"VsmShadowDebug_PS.cso", &ps)) &&
        FAILED(D3DReadFileToBlob(L"Shaders\\Vsm\\VsmShadowDebug_PS.cso", &ps)))
    { printf("VSM: VsmShadowDebug_PS.cso not found\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_shadowDbgRS.Get();
    pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    pd.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_shadowDbgPso))))
    { printf("VSM: shadowDbg PSO failed\n"); return false; }
    return true;
}

void VsmSystem::RenderShadowDebug(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
                                  ID3D12Resource* sceneDepth)
{
    if (!m_valid || !m_shadowDbgPso || !sceneDepth) return;
    GPU_CMD_BEGIN_EVENT(cmd, 200, 100, 200, L"VSM: shadow debug");

    // depth SRV [0] を（毎回）作成
    auto* dev = g_Engine->Device();
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h0 = m_shadowDbgHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {}; s.Format = DXGI_FORMAT_R32_FLOAT;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(sceneDepth, &s, h0);
    }

    // atlas を PIXEL(resting)へ（追跡状態から, no-op if already）。scene深度は DEPTH_WRITE→PIXEL。
    if (m_atlasState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        auto ta = CD3DX12_RESOURCE_BARRIER::Transition(m_atlas.Get(), m_atlasState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &ta);
        m_atlasState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    auto dToSrv = CD3DX12_RESOURCE_BARRIER::Transition(sceneDepth,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &dToSrv);

    cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)m_w, (float)m_h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)m_w, (LONG)m_h };
    cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = { m_shadowDbgHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetPipelineState(m_shadowDbgPso.Get());
    cmd->SetGraphicsRootSignature(m_shadowDbgRS.Get());
    cmd->SetGraphicsRootConstantBufferView(0, GetConstantsAddress());
    cmd->SetGraphicsRootDescriptorTable(1, m_shadowDbgHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    auto dBack = CD3DX12_RESOURCE_BARRIER::Transition(sceneDepth,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &dBack);   // atlas は PIXEL(resting)のまま
    GPU_CMD_END_EVENT(cmd);
}
