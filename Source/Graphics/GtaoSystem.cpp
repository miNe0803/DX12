#include "GtaoSystem.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool GtaoSystem::Init(ID3D12Device* device, UINT width, UINT height)
{
    m_w = width; m_h = height;
    m_aoW = (width + 1) / 2; m_aoH = (height + 1) / 2;   // 半解像度で計算

    // AO 出力（R16_FLOAT, UAV書き込み可, 半解像度）。初期状態 UNORDERED_ACCESS。
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, m_aoW, m_aoH, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_aoTexture))))
            return false;
        GPU_SET_NAME(m_aoTexture.Get(), L"GTAO:AO");
        m_aoState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // 定数バッファ
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(AoCb));
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))))
            return false;
        if (FAILED(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped)))) return false;
        memset(m_cbMapped, 0, sizeof(AoCb));
    }

    // 記述子ヒープ [0]=depth SRV, [1]=AO UAV, [2]=AO SRV
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
        m_srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    D3D12_STATIC_SAMPLER_DESC samp = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    // --- Compute rootsig: b0 CBV, t0 table(depth SRV), u0 table(AO UAV) ---
    {
        CD3DX12_ROOT_PARAMETER params[3] = {};
        params[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &srvR);
        CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        params[2].InitAsDescriptorTable(1, &uavR);
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 3; rs.pParameters = params;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("GTAO CS RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_csRootSig))))
            return false;
    }
    // --- Compute PSO ---
    {
        ComPtr<ID3DBlob> cs;
        if (FAILED(D3DReadFileToBlob(L"GTAO_CS.cso", &cs)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\GTAO_CS.cso", &cs)))
        { printf("GTAO: GTAO_CS.cso not found\n"); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_csRootSig.Get();
        pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_csPso))))
        { printf("GTAO: CS PSO failed\n"); return false; }
    }

    // --- Apply rootsig: b0 CBV, t0 table(AO SRV) ---
    {
        CD3DX12_ROOT_PARAMETER params[2] = {};
        params[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvR; srvR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &srvR, D3D12_SHADER_VISIBILITY_PIXEL);
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2; rs.pParameters = params;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("GTAO Apply RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_applyRootSig))))
            return false;
    }
    // --- Apply PSO: ToneMap_VS + GTAOApply_PS, 乗算ブレンド, 深度無効 ---
    {
        ComPtr<ID3DBlob> vs, ps;
        if (FAILED(D3DReadFileToBlob(L"ToneMap_VS.cso", &vs)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", &vs)))
        { printf("GTAO: ToneMap_VS.cso not found\n"); return false; }
        if (FAILED(D3DReadFileToBlob(L"GTAOApply_PS.cso", &ps)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\GTAOApply_PS.cso", &ps)))
        { printf("GTAO: GTAOApply_PS.cso not found\n"); return false; }

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
        { printf("GTAO: Apply PSO failed\n"); return false; }
    }

    m_valid = true;
    printf("GtaoSystem::Init: OK (%ux%u)\n", m_w, m_h);
    return true;
}

void GtaoSystem::Shutdown() { m_valid = false; }

void GtaoSystem::Execute(ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* depthResource, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvCpu,
    const XMMATRIX& invViewProj, const XMFLOAT3& cameraPos, const Params& params)
{
    if (!m_valid || !depthResource) return;
    auto* dev = g_Engine->Device();
    GPU_CMD_BEGIN_EVENT(cmd, 120, 200, 120, L"GTAO (SSAO)");

    // CB 更新
    {
        auto* cb = reinterpret_cast<AoCb*>(m_cbMapped);
        cb->InvViewProj = XMMatrixTranspose(invViewProj);
        cb->CamPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        cb->InvRes = XMFLOAT2(1.0f / m_aoW, 1.0f / m_aoH);   // AO 計算は半解像度
        cb->Radius = params.radius; cb->Strength = params.strength;
        cb->Bias = params.bias; cb->MaxDist = params.maxDist;
    }

    // 記述子: [0]=depth SRV(R32_FLOAT), [1]=AO UAV, [2]=AO SRV
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_R32_FLOAT; dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; dd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(depthResource, &dd, base);

        D3D12_CPU_DESCRIPTOR_HANDLE h1 = base; h1.ptr += m_srvStride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R16_FLOAT; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(m_aoTexture.Get(), nullptr, &ud, h1);

        D3D12_CPU_DESCRIPTOR_HANDLE h2 = base; h2.ptr += 2 * m_srvStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC ad = {};
        ad.Format = DXGI_FORMAT_R16_FLOAT; ad.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ad.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; ad.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(m_aoTexture.Get(), &ad, h2);
    }

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE gbase = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gDepth = gbase;
    D3D12_GPU_DESCRIPTOR_HANDLE gAoUav = gbase; gAoUav.ptr += m_srvStride;
    D3D12_GPU_DESCRIPTOR_HANDLE gAoSrv = gbase; gAoSrv.ptr += 2 * m_srvStride;

    // (1) 深度 -> NON_PIXEL_SRV, AO -> UAV
    {
        D3D12_RESOURCE_BARRIER b[2]; UINT n = 0;
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (m_aoState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_aoTexture.Get(), m_aoState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(n, b);
        m_aoState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // (2) AO 計算 CS
    cmd->SetComputeRootSignature(m_csRootSig.Get());
    cmd->SetPipelineState(m_csPso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, gDepth);
    cmd->SetComputeRootDescriptorTable(2, gAoUav);
    cmd->Dispatch((m_aoW + 7) / 8, (m_aoH + 7) / 8, 1);   // 半解像度ディスパッチ

    // (3) 深度を戻す, AO -> PIXEL_SRV
    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_aoTexture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(2, b);
        m_aoState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // (4) HDR に乗算適用（フルスクリーン三角, 乗算ブレンド）
    D3D12_VIEWPORT vp = { 0, 0, (float)m_w, (float)m_h, 0, 1 };
    D3D12_RECT sr = { 0, 0, (LONG)m_w, (LONG)m_h };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);
    cmd->OMSetRenderTargets(1, &hdrRtvCpu, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(m_applyRootSig.Get());
    cmd->SetPipelineState(m_applyPso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, gAoSrv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    GPU_CMD_END_EVENT(cmd);
}
