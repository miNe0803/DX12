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
    if (!CreateAtlas(device)) return false;
    if (!CreatePageTable(device, sceneHeap)) return false;
    if (!CreateConstantBuffer(device)) return false;
    if (!CreateRequestResources(device)) return false;
    if (!CreateMarkPipeline(device)) return false;
    if (!CreateAllocResources(device)) return false;
    if (!CreateAllocPipeline(device)) return false;
    if (!CreateBuildResources(device)) return false;
    if (!CreateBuildPipeline(device)) return false;

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

    // 各レベル: 世界範囲 extent=base*2^level、中心をページ世界サイズ格子へスナップ（ワールドロック）
    for (uint32_t i = 0; i < kLevels; ++i)
    {
        float extent = kBaseExtent * (float)(1u << i);
        float pageWorld = extent / (float)kVirtualPagesPerRow;   // 1ページの世界サイズ
        float cx = floorf(camLX / pageWorld) * pageWorld;
        float cy = floorf(camLY / pageWorld) * pageWorld;
        float texelWorld = extent / (float)(kVirtualPagesPerRow * kPageSize);
        c->LevelCenterExtent[i] = XMFLOAT4(cx, cy, extent, texelWorld);
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
    CD3DX12_ROOT_PARAMETER params[3] = {};
    params[0].InitAsConstantBufferView(0);
    CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    params[1].InitAsDescriptorTable(1, &srvR);
    CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    params[2].InitAsDescriptorTable(1, &uavR);
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 3; rs.pParameters = params;
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
    // カウンタ読戻し
    auto rbp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto rbd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));
    for (uint32_t i = 0; i < kCbFrames; ++i)
        if (FAILED(device->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rbd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_counterReadback[i])))) return false;

    // ヒープ [0]=Request SRV,[1]=PageTable UAV,[2]=PhysToVirtual UAV,[3]=Counter UAV
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4;
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
    return true;
}

bool VsmSystem::CreateAllocPipeline(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER params[3] = {};
    params[0].InitAsConstants(4, 0);   // b0: gTotalVirtual, gPhysCap, pad, pad
    CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    params[1].InitAsDescriptorTable(1, &srvR);
    CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);  // u0,u1,u2
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

    // カウンタを 0 クリア（ゼロバッファ先頭4Bをコピー）
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
    uint32_t consts[4] = { kTotalVirtualPages, kPhysicalPages, 0, 0 };
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
