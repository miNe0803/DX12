struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
};

cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

SamplerState smp : register(s0);
Texture2D _TreeMask : register(t0); // R/G/B = 3種の木
Texture2D _NatureMask : register(t1); // R=雪, G=川, B=予備(インヒビタ等)
Texture2D _GroundDiff : register(t2); // 地面ディフューズ
Texture2D _GroundDisp : register(t3); // 地面ディスプレイスメント(高さ)
TextureCube _PrefilterEnv : register(t4);
TextureCube _IrradianceMap : register(t5);
Texture2D _BrdfLut : register(t6);

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[6]; // 0=地面, 1..3=木3種, 4=雪, 5=川
    float4 CameraPos;
    // x: stage 0..3, y: cheap path on, z: grazing threshold, w: near preserve (m, 0=no extra gate)
    float4 DebugParams;
    float4 SunDirection; // .xyz = normalised dir TO light
    float4 SunColor;     // .rgb
};

// Shadow mapping (space2)
Texture2DArray _ShadowMap : register(t0, space2);
SamplerComparisonState shadowSampler : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[3];
    float4 CascadeSplits;
};

static const float kShadowBias = 0.002;

float SampleShadowPCF(float3 worldPos, float viewDepth)
{
    uint cascade = 2;
    if (viewDepth < CascadeSplits.x) cascade = 0;
    else if (viewDepth < CascadeSplits.y) cascade = 1;

    float4 lightClip = mul(float4(worldPos, 1.0), LightVP[cascade]);
    float3 projCoord = lightClip.xyz / lightClip.w;
    float2 shadowUV = projCoord.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    if (shadowUV.x < 0 || shadowUV.x > 1 || shadowUV.y < 0 || shadowUV.y > 1)
        return 1.0;

    float compareDepth = projCoord.z - kShadowBias;
    float shadow = 0;
    const float texelSize = 1.0 / 2048.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        float2 offset = float2(x, y) * texelSize;
        shadow += _ShadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(shadowUV + offset, (float)cascade),
            compareDepth);
    }
    return shadow / 9.0;
};

float3 BlendTreeLayers(float3 mask, float3 ground, float3 tree0, float3 tree1, float3 tree2)
{
    float weightSum = mask.r + mask.g + mask.b;
    if (weightSum <= 1e-4)
        return ground;

    return (tree0 * mask.r + tree1 * mask.g + tree2 * mask.b) / weightSum;
}

float4 main(VSOutput input) : SV_TARGET
{
    float2 uv = input.uv;
    float2 groundUv = uv * 8.0f;
    float3 groundDiff = _GroundDiff.Sample(smp, groundUv).rgb;

    float3 N = input.normal;
    if (length(N) < 0.001f) N = float3(0.0f, 1.0f, 0.0f);
    else N = normalize(N);
    float3 L = normalize(SunDirection.xyz);
    float ndotl = max(dot(N, L), 0.0f);

    // Shadow
    float4 viewPos = mul(float4(input.worldPos, 1.0), View);
    float shadowFactor = SampleShadowPCF(input.worldPos, viewPos.z);

    // 段階デバッグ:
    // 0 = diff のみ
    // 1 = diff + 簡易ライティング
    // 2 = + tree mask 合成
    // 3 = + river/snow 合成（最終）
    int kTerrainDebugStage = clamp((int)round(DebugParams.x), 0, 3);

    float3 albedo = groundDiff;
    if (kTerrainDebugStage == 0)
        return float4(albedo, 1.0f);


    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float nDotV = abs(dot(N, V));
    float grazing = 1.0f - nDotV; // 1 に近いほど接線方向（fill 重い）
    float distToCam = distance(CameraPos.xyz, input.worldPos);

    const float cheapOn = DebugParams.y;
    const float gThresh = DebugParams.z;
    const float nearPreserve = DebugParams.w;
    bool useCheapPath = (cheapOn > 0.5f) && (kTerrainDebugStage >= 3) && (grazing >= gThresh);
    if (useCheapPath && nearPreserve > 0.0f && distToCam < nearPreserve)
        useCheapPath = (grazing >= (gThresh + 0.14f));

    if (useCheapPath)
    {
        float3 cheapLit = albedo * (0.25f + 0.75f * ndotl * shadowFactor);
        return float4(cheapLit, 1.0f);
    }

    float disp = saturate(_GroundDisp.Sample(smp, groundUv).r);
    float3 treeMask = saturate(_TreeMask.Sample(smp, uv).rgb);
    float3 natureMask = saturate(_NatureMask.Sample(smp, uv).rgb);

    // disp はまず明るさ補正だけに使って影響を観察しやすくする
    albedo *= lerp(0.90f, 1.15f, disp);
    float3 lit = albedo * (0.20f + 0.80f * ndotl * shadowFactor);
    if (kTerrainDebugStage == 1)
        return float4(lit, 1.0f);

    // tree mask の寄与（置換ではなく、地面テクスチャを残したまま色を軽く乗せる）
    float treeWeight = saturate(max(treeMask.r, max(treeMask.g, treeMask.b)));
    float3 treeTint = BlendTreeLayers(treeMask, lit, LayerColor[1].rgb, LayerColor[2].rgb, LayerColor[3].rgb);
    float3 c = lerp(lit, lit * treeTint, treeWeight * 0.35f);
    if (kTerrainDebugStage == 2)
        return float4(c, 1.0f);

    // nature mask の寄与も置換ではなく軽い乗算寄与にする
    c = lerp(c, c * LayerColor[5].rgb, natureMask.g * 0.30f); // river
    c = lerp(c, c * LayerColor[4].rgb, natureMask.r * 0.30f); // snow
    return float4(c, 1.0f);
}
