// ============================================================
//  TownFoliagePS.hlsl — 植栽（葉/花/草）専用の軽量PS。
//  葉カードのオーバードローが激しいため、重いフルPBR（IBL/CSM/
//  64灯ループ/POM）を省き、アルベド×簡易ラップライティング＋環境光＋
//  アルファクリップだけにしてピクセルコストを大幅に削減する。
//  ルートシグネチャは TownPS と共通（未使用バインドは無視される）。
// ============================================================

Texture2D    g_Base    : register(t0, space0);
SamplerState g_Sampler : register(s0, space0);

cbuffer SceneCB : register(b1, space0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection;  // .xyz 光へ向かう方向
    float4 SunColor;      // .rgb
    matrix InvViewProj;
};

cbuffer TownParams : register(b2, space0)
{
    float4 Params;
    float4 Params2;
};

struct PS_IN
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD1;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

void main(in PS_IN In, out float4 outColor : SV_Target)
{
    float4 base = g_Base.Sample(g_Sampler, In.uv);
    clip(base.a - 0.33f);   // 葉カットアウト（閾値高めでオーバードロー層を削減）

    float3 albedo = pow(base.rgb, 2.2f);
    float3 N = normalize(In.normal);
    float3 L = normalize(SunDirection.xyz);
    // 薄い葉向けのラップ拡散（両面的な柔らかさ、影計算なし）
    float ndl = saturate(dot(N, L)) * 0.5f + 0.5f;
    float3 color = albedo * (SunColor.rgb * ndl * Params2.w * 0.5f + 0.35f);
    outColor = float4(max(color, 0.0f), 1.0f);   // 線形 HDR
}
