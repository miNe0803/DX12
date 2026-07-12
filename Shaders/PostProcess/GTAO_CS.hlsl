// ============================================================
//  GTAO_CS.hlsl — スクリーン空間アンビエントオクルージョン（GI トラック G0）。
//  シーン深度からワールド座標＋法線を復元し、半球状の遮蔽を推定して AO(R8) を出力。
//  接地感・くぼみ/隅の陰を付け、A4 のフラットな環境光を締める（UE5 の SSAO/GTAO 相当）。
//  適用は GTAOApply_PS が HDR に乗算ブレンド。
// ============================================================

cbuffer AoCb : register(b0)
{
    matrix InvViewProj;   // clip→world（深度からワールド復元）
    float4 CamPos;        // .xyz カメラ世界位置
    float2 InvRes;        // (1/幅, 1/高さ)
    float  Radius;        // サンプル半径（world m 相当のスケール）
    float  Strength;      // AO 強度 0..1
    float  Bias;          // 法線バイアス（自己遮蔽回避）
    float  MaxDist;       // これ以上離れたサンプルは遮蔽に数えない(world m)
    float2 _pad;
};

Texture2D<float>  Depth : register(t0);
RWTexture2D<float> AO    : register(u0);
SamplerState      Smp    : register(s0);

static const int N_SAMPLES = 16;

float3 ReconWorld(float2 uv, float d)
{
    float2 ndc = uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 c = float4(ndc, d, 1.0f);
    float4 w = mul(c, InvViewProj);
    return w.xyz / w.w;
}
float IGN(float2 p) { return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f)))); }
float2 Vogel(int i, int n, float rot)
{
    float r = sqrt((i + 0.5f) / n);
    float t = i * 2.39996323f + rot;
    float s, c; sincos(t, s, c);
    return r * float2(c, s);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5f) * InvRes;
    float d = Depth.SampleLevel(Smp, uv, 0);
    if (d >= 1.0f) { AO[id.xy] = 1.0f; return; }   // 空はAO無し

    float3 P = ReconWorld(uv, d);
    // 深度微分からワールド法線（隣接テクセル）。カメラ側を向く。
    float3 Px = ReconWorld(uv + float2(InvRes.x, 0), Depth.SampleLevel(Smp, uv + float2(InvRes.x, 0), 0)) - P;
    float3 Py = ReconWorld(uv + float2(0, InvRes.y), Depth.SampleLevel(Smp, uv + float2(0, InvRes.y), 0)) - P;
    float3 N = normalize(cross(Px, Py));
    if (dot(N, CamPos.xyz - P) < 0.0f) N = -N;   // カメラ向きへ

    float viewDist = length(P - CamPos.xyz);
    // world 半径 → 画面(uv)半径の近似（角度サイズ ~ Radius/距離）。近距離は広く遠距離は狭く。
    float radiusUV = clamp(Radius / max(viewDist, 0.5f) * 0.5f, InvRes.y * 2.0f, 0.08f);
    float rot = IGN(id.xy) * 6.28318530f;

    float occ = 0.0f;
    [unroll] for (int i = 0; i < N_SAMPLES; ++i)
    {
        float2 suv = uv + Vogel(i, N_SAMPLES, rot) * radiusUV;
        float sd = Depth.SampleLevel(Smp, suv, 0);
        if (sd >= 1.0f) continue;
        float3 S = ReconWorld(suv, sd);
        float3 v = S - P;
        float dist = length(v);
        if (dist < 1e-3f) continue;
        float ndl = dot(N, v / dist);                 // 接平面より上のサンプル＝遮蔽
        float range = saturate(1.0f - dist / MaxDist); // 遠いサンプルは減衰
        occ += saturate(ndl - Bias) * range;
    }
    float ao = saturate(1.0f - (occ / (float)N_SAMPLES) * 2.0f);
    ao = lerp(1.0f, ao, Strength);   // 強度
    AO[id.xy] = ao;
}
