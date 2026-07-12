#include "SsrSystem.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool SsrSystem::Init(ID3D12Device* device, UINT width, UINT height)
{
    m_w = width; m_h = height;

    // HDR コピー（反射のソース）: メイン HDR と同フォーマット
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_hdrCopy))))
            return false;
        GPU_SET_NAME(m_hdrCopy.Get(), L"SSR:HDRCopy");
    }

    // 定数バッファ（SSRParams）
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(SsrCB));
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))))
            return false;
        if (FAILED(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped)))) return false;
        memset(m_cbMapped, 0, sizeof(SsrCB));
    }

    // シェーダ可視 SRV ヒープ（[0]=hdrCopy, [1]=depth, [2]=prefilterCube, [3]=reflection, [4]=puddleNoise）
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 5;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
        m_srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ルートシグネチャ: b0=SceneCB, b1=SSRParams, table t0-t1, s0 linear / s1 point
    {
        CD3DX12_ROOT_PARAMETER params[3] = {};
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsConstantBufferView(1);
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0); // t0,t1,t2,t3,t4
        params[2].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC samp[3] = {};
        samp[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_POINT);
        samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[2] = CD3DX12_STATIC_SAMPLER_DESC(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR);   // 水たまりノイズ用 wrap
        samp[2].AddressU = samp[2].AddressV = samp[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 3; rs.pParameters = params;
        rs.NumStaticSamplers = 3; rs.pStaticSamplers = samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        {
            if (err) printf("SSR RootSig: %s\n", (const char*)err->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
            return false;
    }

    // PSO: フルスクリーン三角形 VS(ToneMap_VS) + SSRResolvePS、ブレンド無し・深度無効
    {
        ComPtr<ID3DBlob> vs, ps;
        if (FAILED(D3DReadFileToBlob(L"ToneMap_VS.cso", &vs)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", &vs)))
        { printf("SSR: ToneMap_VS.cso not found\n"); return false; }
        if (FAILED(D3DReadFileToBlob(L"SSRResolvePS.cso", &ps)))
        { printf("SSR: SSRResolvePS.cso not found\n"); return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSig.Get();
        pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        pd.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
        pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);   // ブレンド無し（全画素上書き）
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso))))
        { printf("SSR: PSO create failed\n"); return false; }
    }

    m_valid = true;
    return true;
}

void SsrSystem::Shutdown() { m_valid = false; }

void SsrSystem::Execute(ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* hdrResource, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvCpu,
    ID3D12Resource* depthResource, ID3D12Resource* prefilterCube,
    ID3D12Resource* reflectionColor, ID3D12Resource* puddleNoise,
    D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddr, const PuddleParams& puddle)
{
    if (!m_valid || !puddle.enabled || !hdrResource || !depthResource || !prefilterCube || !reflectionColor) return;

    GPU_CMD_BEGIN_EVENT(cmd, 90, 150, 210, L"SSR: reflective puddles");
    auto* dev = g_Engine->Device();

    // SSRParams 更新。center は world XZ → シェーダは P0.xz を中心に使う。
    // P0 = (centerX, groundY(参考,未使用), centerZ, enabled)
    m_cbMapped->P0 = XMFLOAT4(puddle.center.x, puddle.groundY, puddle.center.y, 1.0f);
    m_cbMapped->P1 = XMFLOAT4(puddle.half.x, puddle.half.y, puddle.edgeFalloff, puddle.wetDarken);
    m_cbMapped->P2 = XMFLOAT4(puddle.stride, puddle.steps, puddle.thickness, puddle.edgeFade);
    m_cbMapped->Sky = XMFLOAT4(puddle.skyTint.x, puddle.skyTint.y, puddle.skyTint.z, puddle.reflectivity);

    // SRV を毎フレーム生成（[0]=hdrCopy, [1]=depth）
    {
        D3D12_CPU_DESCRIPTOR_HANDLE base = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC cd = {};
        cd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        cd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(m_hdrCopy.Get(), &cd, base);

        D3D12_CPU_DESCRIPTOR_HANDLE ds = base; ds.ptr += m_srvStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_R32_FLOAT;
        dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(depthResource, &dd, ds);

        D3D12_CPU_DESCRIPTOR_HANDLE cs = ds; cs.ptr += m_srvStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC cbd = {};
        cbd.Format = prefilterCube->GetDesc().Format;   // 環境キューブと同フォーマット
        cbd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cbd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cbd.TextureCube.MipLevels = 5;
        dev->CreateShaderResourceView(prefilterCube, &cbd, cs);

        // [3] 平面反射カラー RT（R16G16B16A16_FLOAT, フル解像度）
        D3D12_CPU_DESCRIPTOR_HANDLE rs = cs; rs.ptr += m_srvStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(reflectionColor, &rd, rs);

        // [4] UE5 水たまり分布ノイズ（T_blend_noise_a）。フル mip でサンプル。
        D3D12_CPU_DESCRIPTOR_HANDLE ns = rs; ns.ptr += m_srvStride;
        if (puddleNoise)
        {
            D3D12_RESOURCE_DESC nd0 = puddleNoise->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
            nd.Format = nd0.Format;
            nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nd.Texture2D.MipLevels = nd0.MipLevels ? nd0.MipLevels : 1;
            dev->CreateShaderResourceView(puddleNoise, &nd, ns);
        }
        else
        {
            // フォールバック: reflection を割り当て（未使用時の安全策）
            dev->CreateShaderResourceView(reflectionColor, &rd, ns);
        }
    }

    // (1) HDR -> コピー
    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(hdrResource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(2, b);
        cmd->CopyResource(m_hdrCopy.Get(), hdrResource);
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(hdrResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(2, b);
    }

    // (2) 深度 -> SRV
    auto dToSrv = CD3DX12_RESOURCE_BARRIER::Transition(depthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &dToSrv);

    // (3) フルスクリーン解決パス（HDR RTV へ上書き）
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    D3D12_VIEWPORT vp = { 0, 0, (float)m_w, (float)m_h, 0, 1 };
    D3D12_RECT sr = { 0, 0, (LONG)m_w, (LONG)m_h };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);
    cmd->OMSetRenderTargets(1, &hdrRtvCpu, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, sceneCbAddr);
    cmd->SetGraphicsRootConstantBufferView(1, m_cb->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // (4) 深度を戻す（Atmosphere は DEPTH_WRITE 前提）
    auto dBack = CD3DX12_RESOURCE_BARRIER::Transition(depthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &dBack);

    GPU_CMD_END_EVENT(cmd);
}
