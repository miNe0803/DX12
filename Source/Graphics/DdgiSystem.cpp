#include "DdgiSystem.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <algorithm>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

static const uint32_t kProbeStride = 48;   // sizeof(ProbeSH) = 3*float4

bool DdgiSystem::Init(ID3D12Device* device, const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax)
{
    // --- 固定ワールド格子（建物中心≈原点。バウンスAABBは巨大/歪なので手動キャップ）---
    // 単一カスケード: 48x16x48 @ (4,3,4)m = 192x48x192m を原点中心に。Y は地面付近から手動。
    const uint32_t dx = 48, dy = 16, dz = 48;
    const float sx = 4.0f, sy = 3.0f, sz = 4.0f;
    m_params.gridDims = XMUINT3(dx, dy, dz);
    m_params.gridSpacing = XMFLOAT3(sx, sy, sz);
    float originY = std::clamp(boundsMin.y, -20.0f, 0.0f) - 2.0f;   // 地面やや下から上へ
    m_params.gridOrigin = XMFLOAT3(-(float)(dx - 1) * sx * 0.5f, originY, -(float)(dz - 1) * sz * 0.5f);
    m_params.probeCount = dx * dy * dz;
    m_params.normalBias = 0.3f;      // 町サンプル時のワールド法線オフセット（壁漏れ緩和の初期値）
    m_params.emaAlpha = 0.08f;       // 時間混合率
    m_params.rayCount = 32;          // 1プローブあたりのレイ本数
    m_params.frameIndex = 0;
    m_params.sunDir = XMFLOAT3(0, 1, 0);
    m_params.sunColor = XMFLOAT4(0, 0, 0, 0);

    // --- SH-L1 ピンポン2枚（DEFAULT, UAV可）---
    for (int i = 0; i < 2; ++i)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((UINT64)m_params.probeCount * kProbeStride,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_probeSH[i]))))
            return false;
        GPU_SET_NAME(m_probeSH[i].Get(), i == 0 ? L"DDGI:SH0" : L"DDGI:SH1");
        m_shState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // --- 定数バッファ（UPLOAD, 永続マップ）---
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DdgiCb));
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))))
            return false;
        if (FAILED(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped)))) return false;
        memcpy(m_cbMapped, &m_params, sizeof(DdgiCb));   // 静的params初期値（町が即読めるよう）
    }

    // --- Root sig: b0 CBV, t0 TLAS(rootSRV), t1 prev(rootSRV), u0 cur(rootUAV), table t2-t4 IBL ---
    {
        CD3DX12_DESCRIPTOR_RANGE iblR; iblR.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 2);   // t2-t4
        CD3DX12_ROOT_PARAMETER params[5] = {};
        params[0].InitAsConstantBufferView(0);         // b0
        params[1].InitAsShaderResourceView(0);         // t0 TLAS
        params[2].InitAsShaderResourceView(1);         // t1 prev SH
        params[3].InitAsUnorderedAccessView(0);        // u0 cur SH
        params[4].InitAsDescriptorTable(1, &iblR);     // t2-t4 IBL cubemaps（共有ヒープ）
        D3D12_STATIC_SAMPLER_DESC samp = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 5; rs.pParameters = params;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { if (err) printf("DDGI RootSig: %s\n", (const char*)err->GetBufferPointer()); return false; }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
            return false;
    }
    // --- PSO ---
    {
        ComPtr<ID3DBlob> cs;
        if (FAILED(D3DReadFileToBlob(L"Ddgi_ProbeUpdate_CS.cso", &cs)) &&
            FAILED(D3DReadFileToBlob(L"Shaders\\RT\\Ddgi_ProbeUpdate_CS.cso", &cs)))
        { printf("DDGI: Ddgi_ProbeUpdate_CS.cso not found\n"); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSig.Get();
        pd.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
        { printf("DDGI: PSO failed\n"); return false; }
    }

    m_valid = true;
    printf("DdgiSystem::Init: OK (grid %ux%ux%u=%u probes, origin(%.1f,%.1f,%.1f) spacing(%.1f,%.1f,%.1f))\n",
        dx, dy, dz, m_params.probeCount, m_params.gridOrigin.x, m_params.gridOrigin.y, m_params.gridOrigin.z, sx, sy, sz);
    return true;
}

void DdgiSystem::Shutdown() { m_valid = false; }

void DdgiSystem::Execute(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* sharedHeap,
    D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA, D3D12_GPU_DESCRIPTOR_HANDLE envCubemapGpuHandle,
    const XMFLOAT3& sunColorScaled, const XMFLOAT3& sunDir)
{
    if (!m_valid || tlasGpuVA == 0 || !sharedHeap) return;
    GPU_CMD_BEGIN_EVENT(cmd, 200, 160, 90, L"DDGI probe update");

    const int w = m_write;      // 書き込み
    const int p = 1 - m_write;  // 前フレーム（読み）

    // CB 更新（静的params + 動的 frameIndex/sun）
    m_params.frameIndex = m_frameIndex;
    m_params.sunDir = sunDir;
    m_params.sunColor = XMFLOAT4(sunColorScaled.x, sunColorScaled.y, sunColorScaled.z, 0.0f);
    memcpy(m_cbMapped, &m_params, sizeof(DdgiCb));

    // バリア: cur -> UAV, prev -> NON_PIXEL_SRV
    {
        D3D12_RESOURCE_BARRIER b[2]; UINT n = 0;
        if (m_shState[w] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_probeSH[w].Get(), m_shState[w], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (m_shState[p] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            b[n++] = CD3DX12_RESOURCE_BARRIER::Transition(m_probeSH[p].Get(), m_shState[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (n) cmd->ResourceBarrier(n, b);
        m_shState[w] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_shState[p] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    ID3D12DescriptorHeap* heaps[] = { sharedHeap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(1, tlasGpuVA);
    cmd->SetComputeRootShaderResourceView(2, m_probeSH[p]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_probeSH[w]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(4, envCubemapGpuHandle);
    cmd->Dispatch((m_params.probeCount + 63) / 64, 1, 1);

    // cur を町が読めるよう PIXEL|NON_PIXEL_SRV へ
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(m_probeSH[w].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
        m_shState[w] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    GPU_CMD_END_EVENT(cmd);
    m_write = 1 - m_write;
    ++m_frameIndex;
    if (m_framesAccum < 1000000u) ++m_framesAccum;
}
