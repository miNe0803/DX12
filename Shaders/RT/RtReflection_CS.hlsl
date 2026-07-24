// ============================================================
//  RtReflection_CS.hlsl — DXR-GI 仕上げ: レイトレース反射（inline RayQuery, SM6.6）。
//  各スクリーン画素で深度→ワールド復元し、地面(≈水平)なら水面法線(0,1,0)で反射レイを TLAS へ。
//  反射ヒットは DDGI プローブ更新と同じ陰影（太陽直接光＋DDGI間接＋miss は空prefilter）。
//  出力は half-res RGBA16F。SSRResolvePS が画素自身の screen UV でこれをサンプル＝濡れ地面/水たまりに
//  「画面外ジオメトリも映る」本物の反射（従来の平面ミラーは画面内のみ）。
//  ※ どこに反射が出る/強さ/ブラーは全て既存の SSR 水たまりマスク側が決める。本CSは反射「色」のみ生成。
// ============================================================
#include "Ddgi.hlsli"

cbuffer RtrCB : register(b0)   // クリーンな16Bロー配置（float3 の跨ぎパディング回避）
{
    matrix InvViewProj;   // 0
    float4 CamPos;        // 64
    float3 SunDir;        // 80  太陽へ向かう方向
    float  TMin;          // 92
    float4 SunColor;      // 96  rgb = 太陽色×強度
    float2 InvRes;        // 112 half-res
    float  NormalBias;    // 120
    float  TMax;          // 124
    float  GiIntensity;   // 128
    float  GroundNyMin;   // 132 地面ゲート（法線 y の下限）
    uint   UseDdgi;       // 136
    uint   FrameIndex;    // 140
    float  Roughness;     // 144 光沢の拡がり（0=鏡面）
    uint   RayCount;      // 148 1画素あたりの反射レイ本数（コーンジッタ平均）
    float2 _rtrPad2;      // 152
};
// DDGI 格子 params（Ddgi_SampleField 用）。DdgiSystem の DdgiCb と同レイアウト。
cbuffer DdgiCB : register(b1)
{
    float3 gGridOrigin;  uint  gProbeCount;
    float3 gGridSpacing; uint  gFrameIndex;
    uint3  gGridDims;    float gGridNormalBias;
    float3 gGridSunDir;  float gGridEmaAlpha;
    float4 gGridSunColor;
    uint   gGridRayCount; float gGridIntensity; float2 _gpad;
};

struct GeoInfoR { uint vbIdx; uint ibIdx; uint baseTexIdx; };

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<ProbeSH>       PrevSH : register(t1);
TextureCube g_Prefilter  : register(t2);
TextureCube g_Irradiance : register(t3);
// t4 = brdfLut（テーブル占位・未使用）
StructuredBuffer<GeoInfoR> g_GeoInfo     : register(t5);
StructuredBuffer<uint>     g_InstGeoBase : register(t6);
Texture2D<float>           Depth         : register(t7);
SamplerState  g_Sampler : register(s0);
SamplerState  g_Point   : register(s1);
RWTexture2D<float4> Refl : register(u0);

static const float RTR_PI = 3.14159265359f;
static const uint  RTR_VTX_STRIDE = 84u;   // Position@0, Normal@12, UV@24

float3 ReconWorld(float2 uv, float d)
{
    float2 ndc = uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 c = float4(ndc, d, 1.0f);
    float4 w = mul(c, InvViewProj);
    return w.xyz / w.w;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5f) * InvRes;
    float d = Depth.SampleLevel(g_Point, uv, 0);
    if (d >= 1.0f) { Refl[id.xy] = float4(0, 0, 0, 0); return; }   // 空

    float3 P = ReconWorld(uv, d);

    // 地面ゲート: 中心差分の幾何法線が上向き(≈水平面)でなければ反射不要（壁等は0）。
    float3 pL = ReconWorld(uv - float2(InvRes.x, 0), Depth.SampleLevel(g_Point, uv - float2(InvRes.x, 0), 0));
    float3 pR = ReconWorld(uv + float2(InvRes.x, 0), Depth.SampleLevel(g_Point, uv + float2(InvRes.x, 0), 0));
    float3 pD = ReconWorld(uv - float2(0, InvRes.y), Depth.SampleLevel(g_Point, uv - float2(0, InvRes.y), 0));
    float3 pU = ReconWorld(uv + float2(0, InvRes.y), Depth.SampleLevel(g_Point, uv + float2(0, InvRes.y), 0));
    float3 Ns = normalize(cross(pR - pL, pU - pD));
    if (Ns.y < 0.0f) Ns = -Ns;
    if (Ns.y < GroundNyMin) { Refl[id.xy] = float4(0, 0, 0, 0); return; }

    // 水面反射: 平らな水面法線で鏡面反射（SSR 合成の水モデルと一致）。R2: ラフネスでコーン状にジッタ→平均＝光沢。
    float3 N = float3(0, 1, 0);
    float3 V = normalize(P - CamPos.xyz);
    float3 Rm = reflect(V, N);
    float3 up = abs(Rm.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 Tt = normalize(cross(up, Rm));
    float3 Bt = cross(Rm, Tt);
    float rot = frac(sin(dot(id.xy, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.2831853f;

    uint rc = max(RayCount, 1u);
    float3 Lsum = 0.0f;
    [loop] for (uint ri = 0; ri < rc; ++ri)
    {
        float rr = Roughness * sqrt((ri + 0.5f) / (float)rc);   // コーン半径
        float phi = rot + ri * 2.39996323f;                     // 黄金角
        float3 R = normalize(Rm + (Tt * cos(phi) + Bt * sin(phi)) * rr);

        RayDesc ray;
        ray.Origin = P + N * NormalBias;
        ray.Direction = R;
        ray.TMin = TMin;
        ray.TMax = TMax;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(Scene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFFu, ray);
        q.Proceed();

        float3 L;
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            uint instID = q.CommittedInstanceID();
            uint geomIdx = q.CommittedGeometryIndex();
            uint prim = q.CommittedPrimitiveIndex();
            GeoInfoR gi = g_GeoInfo[g_InstGeoBase[instID] + geomIdx];
            ByteAddressBuffer vb = ResourceDescriptorHeap[gi.vbIdx];
            ByteAddressBuffer ib = ResourceDescriptorHeap[gi.ibIdx];
            uint3 tri = uint3(ib.Load(prim * 12u), ib.Load(prim * 12u + 4u), ib.Load(prim * 12u + 8u));
            float3 p0 = asfloat(vb.Load3(tri.x * RTR_VTX_STRIDE));
            float3 p1 = asfloat(vb.Load3(tri.y * RTR_VTX_STRIDE));
            float3 p2 = asfloat(vb.Load3(tri.z * RTR_VTX_STRIDE));
            float3x4 o2w = q.CommittedObjectToWorld3x4();
            float3 w0 = mul(o2w, float4(p0, 1.0f));
            float3 w1 = mul(o2w, float4(p1, 1.0f));
            float3 w2 = mul(o2w, float4(p2, 1.0f));
            float3 Nhit = normalize(cross(w1 - w0, w2 - w0));
            if (dot(Nhit, -R) < 0.0f) Nhit = -Nhit;

            float2 uv0 = asfloat(vb.Load2(tri.x * RTR_VTX_STRIDE + 24u));
            float2 uv1 = asfloat(vb.Load2(tri.y * RTR_VTX_STRIDE + 24u));
            float2 uv2 = asfloat(vb.Load2(tri.z * RTR_VTX_STRIDE + 24u));
            float2 bc = q.CommittedTriangleBarycentrics();
            float2 hitUV = uv0 * (1.0f - bc.x - bc.y) + uv1 * bc.x + uv2 * bc.y;
            Texture2D hitTex = ResourceDescriptorHeap[gi.baseTexIdx];
            float3 albedo = hitTex.SampleLevel(g_Sampler, hitUV, 2.0f).rgb;

            float3 hitPos = P + R * q.CommittedRayT();
            float NdotL = max(0.0f, dot(Nhit, SunDir));
            float sunVis = 1.0f;
            if (NdotL > 0.0f)
            {
                RayDesc s;
                s.Origin = hitPos + Nhit * 0.05f;
                s.Direction = SunDir;
                s.TMin = 0.02f; s.TMax = 100000.0f;
                RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
                sq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFFu, s);
                sq.Proceed();
                if (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) sunVis = 0.0f;
            }
            float3 sunBounce = (albedo / RTR_PI) * SunColor.rgb * NdotL * sunVis;
            float3 indirect = 0.0f;
            if (UseDdgi != 0)
                indirect = albedo * Ddgi_SampleField(PrevSH, hitPos + Nhit * gGridNormalBias, Nhit,
                                                     gGridOrigin, gGridSpacing, gGridDims) * GiIntensity;
            L = sunBounce + indirect;
        }
        else
        {
            L = g_Prefilter.SampleLevel(g_Sampler, R, 0).rgb;   // 空の反射
        }
        Lsum += min(L, 8.0f);
    }

    Refl[id.xy] = float4(Lsum / (float)rc, 1.0f);
}
