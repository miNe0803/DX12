#include "RtaoSystem.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// 記述子ヒープ レイアウト（毎フレーム再作成）:
//   [0] depth SRV (R32_FLOAT)            レイCS t1 / デノイズ t0
//   [1] rawAO SRV (R16F)                 デノイズ t1
//   [2] prevHist SRV (R16F) = hist[1-w]  デノイズ t2
//   [3] rawAO UAV (R16F)                 レイCS u0
//   [4] curHist UAV (R16F) = hist[w]     デノイズ u0
//   [5] curHist SRV (R16F) = hist[w]     適用   t0
//   ※ デノイズの SRV テーブルは t0..t2 連続 = ヒープ[0,1,2] 連続が必須。
static const UINT kHeapCount = 6;

static bool MakeR16Half(ID3D12Device* dev, UINT w, UINT h, const wchar_t* name, ComPtr<ID3D12Resource>& out)
{
    auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, w, h, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out))))
        return false;
    GPU_SET_NAME(out.Get(), name);
    return true;
}

bool RtaoSystem::Init(ID3D12Device* device, UINT width, UINT height)
{
    m_w = width; m_h = height;
    m_aoW = (width + 1) / 2; m_aoH = (height + 1) / 2;   // 半解像度

    // 生AO + 履歴2枚（R16F, 半解像度, 初期 UNORDERED_ACCESS）
    if (!MakeR16Half(device, m_aoW, m_aoH, L"RTAO:raw", m_rawAo)) return false;
    if (!MakeR16Half(device, m_aoW, m_aoH, L"RTAO:hist0", m_history[0])) return false;
    if (!MakeR16Half(device, m_aoW, m_aoH, L"RTAO:hist1", m_history[1])) return false;
    m_rawState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_histState[0] = m_histState[1] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // 定数バッファ 2 本（レイ/適用共有 + デノイズ）
    auto makeCb = [&](size_t sz, ComPtr<ID3D12Resource>& cb, uint8_t*& mapped) -> bool
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sz);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb)))) return false;
        if (FAILED(cb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        memset(mapped, 0, sz);
        return true;
    };
    if (!makeCb(sizeof(RtaoCb), m_rtCb, m_rtCbMapped)) return false;
    if (!makeCb(sizeof(DenoiseCb), m_dnCb, m_dnCbMapped)) return false;

    // 記述子ヒープ（6）
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kHeapCount;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
        m_srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    D3D12_STATIC_SAMPLER_DESC samps[2];
    samps[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    samps[0].AddressU = samps[0].AddressV = samps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_POINT);
    samps[1].AddressU = samps[1].AddressV = samps[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    // --- Ray-CS rootsig: b0 CBV, t0 TLAS(root SRV), t1 depth(table), u0 rawAO(table) ---
    {
        CD3DX12_ROOT_PARAMETER params[4] = {};
        params[0].InitAsConstantBufferView(0);                       // b0
        params[1].InitAsShaderResourceView(0);                       // t0 = TLAS（ルートSRV）
        CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);  // t1 depth
        params[2].InitAsDescriptorTable(1, &srvR);
        CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // u0 rawAO
        params[3].InitAsDescriptorTable(1, &uavR);
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 4; rs.pParameters = params;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samps[0];
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("RTAO Ray RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rtRootSig))))
            return false;
    }
    // --- Denoise-CS rootsig: b0 CBV, t0..t2 table(depth,rawAO,prevHist), u0 curHist(table) ---
    {
        CD3DX12_ROOT_PARAMETER params[3] = {};
        params[0].InitAsConstantBufferView(0);                       // b0
        CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);  // t0,t1,t2
        params[1].InitAsDescriptorTable(1, &srvR);
        CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // u0
        params[2].InitAsDescriptorTable(1, &uavR);
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 3; rs.pParameters = params;
        rs.NumStaticSamplers = 2; rs.pStaticSamplers = samps;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("RTAO Denoise RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_dnRootSig))))
            return false;
    }
    // --- Apply rootsig: b0 CBV, t0 table(AO SRV) ---（GTAO と同一）
    {
        CD3DX12_ROOT_PARAMETER params[2] = {};
        params[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &srvR, D3D12_SHADER_VISIBILITY_PIXEL);
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2; rs.pParameters = params;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samps[0];
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("RTAO Apply RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_applyRootSig))))
            return false;
    }

    auto loadCs = [&](const wchar_t* bare, const wchar_t* sub, ID3D12RootSignature* rsig, ComPtr<ID3D12PipelineState>& pso) -> bool
    {
        ComPtr<ID3DBlob> cs;
        if (FAILED(D3DReadFileToBlob(bare, &cs)) && FAILED(D3DReadFileToBlob(sub, &cs)))
        { wprintf(L"RTAO: %s not found\n", bare); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rsig;
        pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
        return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    };
    if (!loadCs(L"Rtao_CS.cso", L"Shaders\\RT\\Rtao_CS.cso", m_rtRootSig.Get(), m_rtPso))
    { printf("RTAO: Rtao_CS PSO failed\n"); return false; }
    if (!loadCs(L"RtaoDenoise_CS.cso", L"Shaders\\RT\\RtaoDenoise_CS.cso", m_dnRootSig.Get(), m_dnPso))
    { printf("RTAO: RtaoDenoise_CS PSO failed\n"); return false; }

    // --- Apply PSO: ToneMap_VS + GTAOApply_PS, 乗算ブレンド, 深度無効 ---（GTAO の .cso を再利用）
    {
        ComPtr<ID3DBlob> vs, ps;
        if (FAILED(D3DReadFileToBlob(L"ToneMap_VS.cso", &vs)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", &vs)))
        { printf("RTAO: ToneMap_VS.cso not found\n"); return false; }
        if (FAILED(D3DReadFileToBlob(L"GTAOApply_PS.cso", &ps)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\GTAOApply_PS.cso", &ps)))
        { printf("RTAO: GTAOApply_PS.cso not found\n"); return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_applyRootSig.Get();
        pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        pd.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
        pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        auto& rt0 = pd.BlendState.RenderTarget[0];
        rt0.BlendEnable = TRUE;                    // 乗算: HDR *= ao
        rt0.SrcBlend = D3D12_BLEND_DEST_COLOR;
        rt0.DestBlend = D3D12_BLEND_ZERO;
        rt0.BlendOp = D3D12_BLEND_OP_ADD;
        rt0.SrcBlendAlpha = D3D12_BLEND_ZERO;
        rt0.DestBlendAlpha = D3D12_BLEND_ONE;
        rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_applyPso))))
        { printf("RTAO: Apply PSO failed\n"); return false; }
    }

    m_valid = true;
    printf("RtaoSystem::Init: OK (%ux%u, AO %ux%u)\n", m_w, m_h, m_aoW, m_aoH);
    return true;
}

void RtaoSystem::Shutdown() { m_valid = false; }

// R16F SRV / UAV を base+idx*stride の位置へ作る小ヘルパ（CPU 側）
static void MakeSrvR16(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R16_FLOAT; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(res, &d, h);
}
static void MakeUavR16(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R16_FLOAT; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(res, nullptr, &d, h);
}

void RtaoSystem::Execute(ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* depthResource, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvCpu,
    D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA,
    const XMMATRIX& invViewProj, const XMFLOAT3& cameraPos, const Params& params)
{
    if (!m_valid || !depthResource || tlasGpuVA == 0) return;
    auto* dev = g_Engine->Device();
    GPU_CMD_BEGIN_EVENT(cmd, 90, 160, 220, L"RTAO (ray-traced AO)");

    const int w = m_histWrite;          // 今フレーム書き込む履歴
    const int p = 1 - m_histWrite;      // 前フレーム履歴（読み）

    // CB 更新
    {
        auto* cb = reinterpret_cast<RtaoCb*>(m_rtCbMapped);
        cb->InvViewProj = XMMatrixTranspose(invViewProj);
        cb->CamPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        cb->InvRes = XMFLOAT2(1.0f / m_aoW, 1.0f / m_aoH);
        cb->Radius = params.radius;
        cb->NormalBias = params.normalBias;
        cb->TMin = params.tMin;
        cb->RayCount = params.rayCount;
        cb->Strength = params.strength;
        cb->FrameIndex = m_frameIndex;

        auto* dn = reinterpret_cast<DenoiseCb*>(m_dnCbMapped);
        dn->InvViewProj = XMMatrixTranspose(invViewProj);
        dn->PrevViewProj = XMMatrixTranspose(m_prevViewProj);
        dn->InvRes = XMFLOAT2(1.0f / m_aoW, 1.0f / m_aoH);
        dn->BlendAlpha = params.blendAlpha;
        dn->HasHistory = m_hasPrev ? 1u : 0u;
    }

    // 記述子作成（6）
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpuAt = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)i * m_srvStride; return h; };
    {
        // [0] depth SRV (R32F)
        D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_R32_FLOAT; dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; dd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(depthResource, &dd, cpuAt(0));
        MakeSrvR16(dev, m_rawAo.Get(), cpuAt(1));          // [1] rawAO SRV
        MakeSrvR16(dev, m_history[p].Get(), cpuAt(2));     // [2] prevHist SRV
        MakeUavR16(dev, m_rawAo.Get(), cpuAt(3));          // [3] rawAO UAV
        MakeUavR16(dev, m_history[w].Get(), cpuAt(4));     // [4] curHist UAV
        MakeSrvR16(dev, m_history[w].Get(), cpuAt(5));     // [5] curHist SRV
    }
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE gbase = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuAt = [&](UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE h = gbase; h.ptr += (UINT64)i * m_srvStride; return h; };

    // (B1) depth->NON_PIXEL_SRV, rawAO->UAV, curHist->UAV, prevHist->NON_PIXEL_SRV
    {
        D3D12_RESOURCE_BARRIER b[4]; UINT n = 0;
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (m_rawState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_rawAo.Get(), m_rawState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (m_histState[w] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_history[w].Get(), m_histState[w], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (m_histState[p] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_history[p].Get(), m_histState[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(n, b);
        m_rawState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_histState[w] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_histState[p] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    // (Ray CS) 生AO を書く
    cmd->SetComputeRootSignature(m_rtRootSig.Get());
    cmd->SetPipelineState(m_rtPso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_rtCb->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(1, tlasGpuVA);   // t0 = TLAS
    cmd->SetComputeRootDescriptorTable(2, gpuAt(0));       // t1 depth
    cmd->SetComputeRootDescriptorTable(3, gpuAt(3));       // u0 rawAO
    cmd->Dispatch((m_aoW + 7) / 8, (m_aoH + 7) / 8, 1);

    // (B2) rawAO UAV->NON_PIXEL_SRV
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(m_rawAo.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
        m_rawState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    // (Denoise CS) 前フレーム再投影 + 混合 → curHist
    cmd->SetComputeRootSignature(m_dnRootSig.Get());
    cmd->SetPipelineState(m_dnPso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_dnCb->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, gpuAt(0));       // t0..t2 (depth,rawAO,prevHist) 連続
    cmd->SetComputeRootDescriptorTable(2, gpuAt(4));       // u0 curHist
    cmd->Dispatch((m_aoW + 7) / 8, (m_aoH + 7) / 8, 1);

    // (B3) depth 復帰, curHist UAV->PIXEL_SRV
    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_history[w].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(2, b);
        m_histState[w] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // (Apply) HDR に乗算適用（フルスクリーン三角, 3x3ブラー = GTAOApply_PS, curHist を読む）
    D3D12_VIEWPORT vp = { 0, 0, (float)m_w, (float)m_h, 0, 1 };
    D3D12_RECT sr = { 0, 0, (LONG)m_w, (LONG)m_h };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);
    cmd->OMSetRenderTargets(1, &hdrRtvCpu, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(m_applyRootSig.Get());
    cmd->SetPipelineState(m_applyPso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_rtCb->GetGPUVirtualAddress());   // InvRes 用
    cmd->SetGraphicsRootDescriptorTable(1, gpuAt(5));      // curHist SRV
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    GPU_CMD_END_EVENT(cmd);

    // 次フレーム用の記録
    m_prevViewProj = XMMatrixInverse(nullptr, invViewProj);   // 今フレームの VP（未転置）
    m_hasPrev = true;
    m_histWrite = 1 - m_histWrite;
    ++m_frameIndex;
}
