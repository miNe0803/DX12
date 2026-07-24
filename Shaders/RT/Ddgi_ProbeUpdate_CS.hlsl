// ============================================================
//  Ddgi_ProbeUpdate_CS.hlsl — DXR-GI Phase G: プローブ更新（inline RayQuery, SM6.6）。
//  1スレッド=1プローブ。全球へ R 本のレイを飛ばし、入射放射輝度を SH-L1 へ投影、
//  前フレーム係数と EMA 混合して書き込む。町は静的なのでワールド固定格子で安定。
//    G-a: MISS=空(prefilter mip0), HIT=0（＝空遮蔽GI, ゼロ新規データ）。
//    G-b: HIT=太陽直接光（ヒット法線を GeometryInfo で取得）を追加予定。
//    G-c: HIT に前フレームプローブの自己照明（多重バウンス）を追加予定。
// ============================================================
#include "Ddgi.hlsli"

cbuffer DdgiCB : register(b0)
{
    float3 gGridOrigin;  uint  gProbeCount;
    float3 gGridSpacing; uint  gFrameIndex;
    uint3  gGridDims;    float gNormalBias;
    float3 gSunDir;      float gEmaAlpha;    // gSunDir: 太陽へ向かう方向（G-b用）
    float4 gSunColor;                        // rgb = 太陽色×強度（G-b用）
    uint   gRayCount;    float3 _pad;
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<ProbeSH>       PrevSH : register(t1);
RWStructuredBuffer<ProbeSH>     CurSH  : register(u0);
// IBL cubemap テーブル（base=s_envCubemapHandle）: t2=prefilter, t3=irradiance, t4=brdfLut。
TextureCube g_Prefilter  : register(t2);
TextureCube g_Irradiance : register(t3);
SamplerState g_Sampler   : register(s0);

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}
float2 Hammersley(uint i, uint n) { return float2((float)i / (float)n, RadicalInverseVdC(i)); }
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= gProbeCount) return;

    uint3 dims = gGridDims;
    uint3 coord;
    coord.x = idx % dims.x;
    coord.y = (idx / dims.x) % dims.y;
    coord.z = idx / (dims.x * dims.y);
    float3 P = Ddgi_ProbeWorldPos(coord, gGridOrigin, gGridSpacing);

    // プローブ毎 + フレーム毎に別分散（黄金比前進）で全球を漸進積分。
    float2 cp = float2(Hash12((float2)coord.xy + coord.z * 3.17f), Hash12((float2)coord.yz + 7.13f));
    cp = frac(cp + (float)gFrameIndex * float2(0.7548776662f, 0.5698402909f));

    uint rc = max(gRayCount, 1u);
    ProbeSH acc = SH_Zero();
    [loop] for (uint i = 0; i < rc; ++i)
    {
        float2 u = frac(Hammersley(i, rc) + cp);
        float z = 1.0f - 2.0f * u.x;                 // 全球一様
        float r = sqrt(max(0.0f, 1.0f - z * z));
        float phi = 6.28318530718f * u.y;
        float3 dir = float3(r * cos(phi), r * sin(phi), z);

        RayDesc ray;
        ray.Origin = P;
        ray.Direction = dir;
        ray.TMin = 0.05f;        // コプラナ自己交差回避（道路×地形）
        ray.TMax = 100000.0f;

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(Scene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFFu, ray);
        q.Proceed();

        float3 L;
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            L = 0.0f;                                          // G-a: ヒットは無寄与（G-bで太陽バウンス）
        else
            L = g_Prefilter.SampleLevel(g_Sampler, dir, 0).rgb; // 空の生放射輝度（roughness0=生env）
        SH_ProjectAddRadiance(acc, dir, L);
    }
    // モンテカルロ積分の重み（全球 4π / N）
    acc = SH_Scale(acc, (4.0f * 3.14159265359f) / (float)rc);

    // 前フレームと EMA 混合（ワールド固定なので時間平均で安定収束）
    ProbeSH prev = PrevSH[idx];
    CurSH[idx] = SH_Lerp(prev, acc, gEmaAlpha);
}
