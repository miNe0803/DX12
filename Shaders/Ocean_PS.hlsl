// ============================================================
// Ocean Pixel Shader
// 海面 / 巨大水面用。Water_PS と違って川マスクで discard しない。
// 常に「深部の水」として描画 → 地平線まで水を伸ばす。
// ============================================================

struct VSOutput
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 normal   : NORMAL;
};

cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[6];
    float4 CameraPos;
    float4 DebugParams;
    float4 SunDirection;
    float4 SunColor;
};

SamplerState smp : register(s0);

// terrain ルートシグに合わせる
Texture2D _TreeMask    : register(t0);
Texture2D _NatureMask  : register(t1);
Texture2D _GroundDiff  : register(t2);
Texture2D _GroundDisp  : register(t3);
Texture2D _RiversMask  : register(t4);
Texture2D _SnowMask    : register(t5);
TextureCube _PrefilterEnv  : register(t6);
TextureCube _IrradianceMap : register(t7);
Texture2D _BrdfLut         : register(t8);
Texture2D _RiversDirection : register(t9);
Texture2D _WaterColorTint  : register(t10);
Texture2D _FreshWaterMask  : register(t11);
Texture2D _InhibitorsMask  : register(t12);

// Shadow (space2)
Texture2DArray _ShadowMap : register(t0, space2);
SamplerComparisonState shadowSampler : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4];
    float4 CascadeSplits;
};

float4 main(VSOutput input) : SV_TARGET
{
    // *** デバッグ: Ocean (海) パスは純緑 ***
    return float4(0.0, 1.0, 0.0, 1.0);

    float time = CameraPos.w;
    float wrappedTime = fmod(time, 500.0);

    // 海なので全画素「深い」水として扱う
    float waterDepth01 = 0.90;

    // 流れ方向は固定 (静かな海)。
    float2 flowDir = float2(1.0, 0.4);
    flowDir = normalize(flowDir);

    // 手続き的さざ波
    float2 wp = input.worldPos.xz;
    float t1 = wrappedTime * 0.40;
    float t2 = wrappedTime * 0.70;
    float2 d1 = flowDir;
    float2 d2 = float2(-flowDir.y, flowDir.x);
    float wave1 = sin(dot(wp, d1) * 0.18 + t1);
    float wave2 = sin(dot(wp, d2) * 0.30 + t2) * 0.55;
    float2 waveGrad = float2(
        cos(dot(wp, d1) * 0.18 + t1) * 0.18 * d1.x +
        cos(dot(wp, d2) * 0.30 + t2) * 0.30 * 0.55 * d2.x,
        cos(dot(wp, d1) * 0.18 + t1) * 0.18 * d1.y +
        cos(dot(wp, d2) * 0.30 + t2) * 0.30 * 0.55 * d2.y) * 0.08;
    float3 waterNormal = normalize(float3(waveGrad.x, 1.0, waveGrad.y));

    // PBR 反射
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float NdotV = saturate(dot(waterNormal, V));
    const float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);
    float3 R = reflect(-V, waterNormal);
    float3 reflection = _PrefilterEnv.SampleLevel(smp, R, 0.5).rgb;

    // 深い海色のグラデ
    float3 shallowColor = float3(0.20, 0.55, 0.65);
    float3 deepColor    = float3(0.04, 0.18, 0.32);
    float3 waterBase = lerp(shallowColor, deepColor, 0.6);

    // 合成
    float3 waterColor = lerp(waterBase, reflection, fresnel);

    // 太陽スペキュラ
    float3 L = normalize(SunDirection.xyz);
    float3 H = normalize(L + V);
    float NdotH = saturate(dot(waterNormal, H));
    float sunSpec = pow(NdotH, 256.0) * fresnel * 2.5;
    waterColor += SunColor.rgb * sunSpec;

    float alpha = 0.96;
    return float4(waterColor * alpha, alpha);
}
