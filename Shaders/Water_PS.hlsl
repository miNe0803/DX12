// ============================================================
// Water Pixel Shader
// Renders realistic water surfaces using:
// - Dual-layer scrolling normal maps
// - Fresnel reflection/refraction
// - Beer's Law depth absorption
// - Shore foam
// - Sun specular highlight
// ============================================================

struct VSOutput
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 normal   : NORMAL;
};

cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection;
    float4 SunColor;
    matrix InvViewProj;
};

cbuffer WaterCB : register(b1)
{
    float4 WaterParams;     // x=time, y=waterSurfaceY, z=foamWidth, w=specPower
    float4 AbsorptionCoeff; // RGB absorption (Beer's Law), w=absorptionScale
    float4 WaterColor;      // shallow water tint
    float4 NormalScroll1;   // xy=direction, z=speed, w=uvScale
    float4 NormalScroll2;   // xy=direction, z=speed, w=uvScale
};

SamplerState smp : register(s0);

// Textures (will be bindless indices in final pipeline; for now use explicit slots)
Texture2D _WaterNormal1 : register(t0); // seamless water normal map A
Texture2D _WaterNormal2 : register(t1); // seamless water normal map B
Texture2D _SceneColor   : register(t2); // opaque pass result (for refraction)
Texture2D _SceneDepth   : register(t3); // main depth buffer as R32_FLOAT
TextureCube _EnvMap      : register(t4); // IBL environment for reflection fallback

// Shadow (reuse from main pass)
Texture2DArray _ShadowMap : register(t0, space2);
SamplerComparisonState shadowSampler : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4];
    float4 CascadeSplits;
};

static const float kShadowBias = 0.002;

float SampleShadowPCF(float3 worldPos, float viewDepth)
{
    if (viewDepth >= CascadeSplits.x)
        return 1.0;
    uint cascade = 0;

    float4 lc = mul(float4(worldPos, 1.0), LightVP[cascade]);
    float3 pc = lc.xyz / lc.w;
    float2 suv = pc.xy * 0.5 + 0.5;
    suv.y = 1.0 - suv.y;
    if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1) return 1.0;

    float cmp = pc.z - kShadowBias;
    float shadow = 0;
    float ts = 1.0 / 512.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(suv + float2(x,y)*ts, (float)cascade), cmp);
    return shadow / 9.0;
}

float3 ReconstructWorldPos(float2 screenUV, float depth)
{
    float2 ndc = screenUV * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

float4 main(VSOutput input) : SV_TARGET
{
    float time = WaterParams.x;
    float foamWidth = WaterParams.z;
    float specPower = WaterParams.w;

    // --- 1. Dual-layer scrolling normals ---
    float2 uv1 = input.uv * NormalScroll1.w + NormalScroll1.xy * NormalScroll1.z * time;
    float2 uv2 = input.uv * NormalScroll2.w + NormalScroll2.xy * NormalScroll2.z * time;
    float3 n1 = _WaterNormal1.Sample(smp, uv1).xyz * 2.0 - 1.0;
    float3 n2 = _WaterNormal2.Sample(smp, uv2).xyz * 2.0 - 1.0;
    float3 waterNormal = normalize(float3(n1.xy + n2.xy, n1.z));

    // Blend with geometric normal
    float3 N = normalize(input.normal);
    float3 T = normalize(cross(N, float3(0, 0, 1)));
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);
    float3 worldNormal = normalize(mul(waterNormal, TBN));

    float3 V = normalize(CameraWorld.xyz - input.worldPos);
    float NdotV = saturate(dot(worldNormal, V));

    // --- 2. Fresnel ---
    float F0 = 0.02; // water IOR 1.33
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    // --- 3. Reflection ---
    float3 R = reflect(-V, worldNormal);
    float3 reflection = _EnvMap.SampleLevel(smp, R, 0.0).rgb;
    // TODO: replace with DXR RT reflection texture when Step 6 is integrated

    // --- 4. Refraction (Beer's Law depth absorption) ---
    float2 screenUV = input.svpos.xy / float2(1920.0, 1080.0); // TODO: pass screen dimensions
    float sceneDepthRaw = _SceneDepth.Sample(smp, screenUV).r;
    float3 sceneWorldPos = ReconstructWorldPos(screenUV, sceneDepthRaw);
    float waterDepth = max(0.0, input.worldPos.y - sceneWorldPos.y);

    // Distort refraction UV by water normal
    float2 refractionUV = screenUV + worldNormal.xz * 0.02 * saturate(waterDepth);
    float3 refractionColor = _SceneColor.Sample(smp, refractionUV).rgb;

    // Beer's Law absorption
    float3 absorption = exp(-waterDepth * AbsorptionCoeff.xyz * AbsorptionCoeff.w);
    float3 refraction = lerp(WaterColor.rgb, refractionColor, absorption);

    // --- 5. Combine reflection + refraction ---
    float3 waterColor = lerp(refraction, reflection, fresnel);

    // --- 6. Sun specular ---
    float3 L = normalize(SunDirection.xyz);
    float3 H = normalize(L + V);
    float NdotH = max(dot(worldNormal, H), 0.0);
    float sunSpec = pow(NdotH, specPower) * fresnel;

    float4 viewPos = mul(float4(input.worldPos, 1.0), View);
    float shadowFactor = SampleShadowPCF(input.worldPos, viewPos.z);
    waterColor += SunColor.rgb * sunSpec * shadowFactor;

    // --- 7. Shore foam ---
    float foam = saturate(1.0 - waterDepth / foamWidth);
    foam *= foam; // sharper falloff
    waterColor = lerp(waterColor, float3(0.9, 0.95, 1.0), foam * 0.6);

    return float4(waterColor, 1.0);
}
