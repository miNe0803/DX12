#include "RtReflectionSystem.h"
#include "DescriptorHeap.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

bool RtReflectionSystem::Init(ID3D12Device* device, DescriptorHeap* sharedHeap, UINT fullW, UINT fullH)
{
    if (!device || !sharedHeap) return false;
    m_heap = sharedHeap;
    m_fullW = fullW; m_fullH = fullH;
    m_halfW = (fullW + 1) / 2; m_halfH = (fullH + 1) / 2;

    // half-res RGBA16F 反射ターゲット（UAV書き→SRV読み）
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_halfW, m_halfH, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_target))))
            return false;
        GPU_SET_NAME(m_target.Get(), L"RTR:reflection");
        m_targetState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // 共有ヒープに depth SRV + 反射 UAV を確保（1ヒープで bindless + テーブルを解決）
    UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = sharedHeap->GetHeap()->GetGPUDescriptorHandleForHeapStart();
    {
        m_depthSrvIdx = sharedHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
        sharedHeap->CreateSRVAt(m_depthSrvIdx, g_Engine->GetDepthStencilResource(), sd);
        m_depthSrvGpu.ptr = gpuStart.ptr + (UINT64)m_depthSrvIdx * inc;

        m_reflUavIdx = sharedHeap->AllocateIndex();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        sharedHeap->CreateUAVAt(m_reflUavIdx, m_target.Get(), ud);
        m_reflUavGpu.ptr = gpuStart.ptr + (UINT64)m_reflUavIdx * inc;
    }

    // CB + ダミー（DDGI-off 時に b1/t1 を満たす）
    auto mkBuf = [&](size_t sz, ComPtr<ID3D12Resource>& out, uint8_t** mapped) -> bool
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sz);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out)))) return false;
        void* p = nullptr;
        if (FAILED(out->Map(0, nullptr, &p))) return false;
        memset(p, 0, sz);
        if (mapped) *mapped = reinterpret_cast<uint8_t*>(p); else out->Unmap(0, nullptr);
        return true;
    };
    if (!mkBuf(sizeof(RtrCb), m_cb, &m_cbMapped)) return false;
    if (!mkBuf(96, m_dummyDdgiCb, nullptr)) return false;   // ゼロ DdgiCb
    if (!mkBuf(48, m_dummySH, nullptr)) return false;        // ゼロ SH

    // Root sig: b0 RtrCB, b1 DdgiCB, t0 TLAS, t1 PrevSH, t5 GeoInfo, t6 InstGeoBase,
    //           table t2-t4 env, table t7 depth, table u0 refl。 + HEAP_DIRECTLY_INDEXED。
    {
        CD3DX12_DESCRIPTOR_RANGE envR; envR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 2);   // t2-t4
        CD3DX12_DESCRIPTOR_RANGE depthR; depthR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7); // t7
        CD3DX12_DESCRIPTOR_RANGE uavR; uavR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);    // u0
        CD3DX12_ROOT_PARAMETER p[9] = {};
        p[0].InitAsConstantBufferView(0);
        p[1].InitAsConstantBufferView(1);
        p[2].InitAsShaderResourceView(0);
        p[3].InitAsShaderResourceView(1);
        p[4].InitAsShaderResourceView(5);
        p[5].InitAsShaderResourceView(6);
        p[6].InitAsDescriptorTable(1, &envR);
        p[7].InitAsDescriptorTable(1, &depthR);
        p[8].InitAsDescriptorTable(1, &uavR);
        D3D12_STATIC_SAMPLER_DESC samp[2];
        samp[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_POINT);
        samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 9; rs.pParameters = p;
        rs.NumStaticSamplers = 2; rs.pStaticSamplers = samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("RTR RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
            return false;
    }
    {
        ComPtr<ID3DBlob> cs;
        if (FAILED(D3DReadFileToBlob(L"RtReflection_CS.cso", &cs)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\RT\\RtReflection_CS.cso", &cs)))
        { printf("RTR: RtReflection_CS.cso not found\n"); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSig.Get();
        pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
        { printf("RTR: PSO failed\n"); return false; }
    }

    m_valid = true;
    printf("RtReflectionSystem::Init: OK (refl %ux%u RGBA16F)\n", m_halfW, m_halfH);
    return true;
}

void RtReflectionSystem::Shutdown() { m_valid = false; }

void RtReflectionSystem::Execute(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depthResource,
    D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA, D3D12_GPU_DESCRIPTOR_HANDLE envCubemapGpuHandle,
    D3D12_GPU_VIRTUAL_ADDRESS geomInfoVA, D3D12_GPU_VIRTUAL_ADDRESS instGeoBaseVA,
    D3D12_GPU_VIRTUAL_ADDRESS ddgiCbVA, D3D12_GPU_VIRTUAL_ADDRESS ddgiSHReadVA, bool ddgiReady,
    const XMMATRIX& invViewProj, const XMFLOAT3& cameraPos,
    const XMFLOAT3& sunColorScaled, const XMFLOAT3& sunDir, float giIntensity)
{
    if (!m_valid || !depthResource || tlasGpuVA == 0 || geomInfoVA == 0 || instGeoBaseVA == 0) return;
    GPU_CMD_BEGIN_EVENT(cmd, 120, 180, 240, L"RT Reflections");

    const bool useDdgi = (ddgiSHReadVA != 0 && ddgiCbVA != 0 && ddgiReady);

    // CB 更新
    {
        auto* cb = reinterpret_cast<RtrCb*>(m_cbMapped);
        cb->InvViewProj = XMMatrixTranspose(invViewProj);
        cb->CamPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        cb->SunDir = sunDir;
        cb->TMin = 0.03f;
        cb->SunColor = XMFLOAT4(sunColorScaled.x, sunColorScaled.y, sunColorScaled.z, 0.0f);
        cb->InvRes = XMFLOAT2(1.0f / m_halfW, 1.0f / m_halfH);
        cb->NormalBias = 0.05f;
        cb->TMax = 100000.0f;
        cb->GiIntensity = giIntensity;
        cb->GroundNyMin = 0.5f;   // 上向き≈水平のみ反射（壁は除外）
        cb->UseDdgi = useDdgi ? 1u : 0u;
        cb->FrameIndex = m_frameIndex;
        cb->Roughness = 0.06f;    // R2: 濡れ路面の軽い光沢拡がり
        cb->RayCount = 4u;        // コーンジッタ平均本数
    }

    // バリア: refl -> UAV, depth -> NON_PIXEL_SRV
    {
        D3D12_RESOURCE_BARRIER b[2]; UINT n = 0;
        if (m_targetState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_target.Get(), m_targetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(n, b);
        m_targetState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    ID3D12DescriptorHeap* heaps[] = { m_heap->GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(1, useDdgi ? ddgiCbVA : m_dummyDdgiCb->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, tlasGpuVA);
    cmd->SetComputeRootShaderResourceView(3, useDdgi ? ddgiSHReadVA : m_dummySH->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(4, geomInfoVA);
    cmd->SetComputeRootShaderResourceView(5, instGeoBaseVA);
    cmd->SetComputeRootDescriptorTable(6, envCubemapGpuHandle);
    cmd->SetComputeRootDescriptorTable(7, m_depthSrvGpu);
    cmd->SetComputeRootDescriptorTable(8, m_reflUavGpu);
    cmd->Dispatch((m_halfW + 7) / 8, (m_halfH + 7) / 8, 1);

    // バリア: refl -> PIXEL_SRV（SsrSystem が読む）, depth 復帰
    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_target.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmd->ResourceBarrier(2, b);
        m_targetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    GPU_CMD_END_EVENT(cmd);
    ++m_frameIndex;
}
