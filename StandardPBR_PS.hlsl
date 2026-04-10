// --- [VS output struct] ---
struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
    float4 nprPerMesh : TEXCOORD2;
};


SamplerState smp : register(s0);
Texture2D _AlbedoMap : register(t0, space0);
Texture2D _NormalMap : register(t1, space0);
Texture2D _MetallicMap : register(t2, space0);
Texture2D _RoughnessMap : register(t3, space0);
Texture2D _RampTex : register(t4, space0);
Texture2D _SphereMap : register(t5, space0);
TextureCube _PrefilterEnv  : register(t6, space0);
TextureCube _IrradianceMap : register(t7, space0);
Texture2D _BrdfLut         : register(t8, space0);

cbuffer SceneCB : register(b0, space0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection; // .xyz = normalised dir TO light, .w = intensity
    float4 SunColor;     // .rgb
    matrix InvViewProj;
};

cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams;
    float4 CameraPos;
    float4 NprTuning;
    float4 NprTuning2;
    float4 NprDebugHdr;
};

// Shadow mapping (space2)
Texture2DArray _ShadowMap : register(t0, space2);
SamplerComparisonState shadowSampler : register(s1, space2);

cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4]; // 4-cascade CSM
    float4 CascadeSplits; // .x/.y/.z/.w = view-space far for cascade 0/1/2/3
};

static const float kShadowBias = 0.001;

float SampleShadowPCF(float3 worldPos, float viewDepth)
{
    uint cascade = 3;
    if      (viewDepth < CascadeSplits.x) cascade = 0;
    else if (viewDepth < CascadeSplits.y) cascade = 1;
    else if (viewDepth < CascadeSplits.z) cascade = 2;

    float4 lightClip = mul(float4(worldPos, 1.0), LightVP[cascade]);
    float3 projCoord = lightClip.xyz / lightClip.w;
    float2 shadowUV = projCoord.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    if (shadowUV.x < 0 || shadowUV.x > 1 || shadowUV.y < 0 || shadowUV.y > 1)
        return 1.0;

    float compareDepth = projCoord.z - kShadowBias;

    // 3x3 PCF
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
}

// --- [Pixel shader main] ---
float4 main(VSOutput input) : SV_TARGET
{
    // 1. Sample textures（質感はピクセル単位でマップから自動判別）
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    // Assimp 焼き込み: PMX 等のマテリアル Diffuse（頂点カラー）× テクスチャ
    albedo *= input.color;
    albedo.rgb = pow(albedo.rgb, 2.2);
    // PMX スフィア（NprPerMesh.y = sphereMode: 1=乗算 2+=加算）
    int sphMode = (int)floor(input.nprPerMesh.y + 0.5f);
    if (sphMode > 0)
    {
        float4 sph = _SphereMap.Sample(smp, input.uv);
        sph.rgb = pow(max(sph.rgb, 1e-5), 2.2);
        if (sphMode == 1)
            albedo.rgb *= sph.rgb;
        else
            albedo.rgb += sph.rgb;
    }
    float4 nSample = _NormalMap.Sample(smp, input.uv);
    float metallicRaw = _MetallicMap.Sample(smp, input.uv).r;
    float roughness =_RoughnessMap.Sample(smp, input.uv).r;
    // nprPerMesh.w > 0.5: tree — MetallicMap slot holds alpha mask for leaves
    float metallic = metallicRaw;
    if (input.nprPerMesh.w > 0.5)
    {
        clip(metallicRaw - 0.3);
        metallic = 0.0;
    }
    if (metallic >= 0.98 && roughness >= 0.98)
    {
        metallic = 0.0;
        roughness = 0.92;
    }
    roughness = max(roughness, 0.04);

    // 2. TBN matrix (tangent -> world)
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    // 3. Normal map decode; nprPerMesh.w > 0.5 → OpenGL normal map (flip Y)
    float3 decodedNormal = nSample.rgb * 2.0 - 1.0;
    if (input.nprPerMesh.w > 0.5)
        decodedNormal.y = -decodedNormal.y;
    float nScale = (RimParams.y > 0.001) ? RimParams.y : 1.0;
    float3 normalTS = normalize(float3(0, 0, 1) + (decodedNormal - float3(0, 0, 1)) * nScale);
    float3 worldNormal = normalize(mul(normalTS, TBN));

    // 4. Direct lighting (data-driven from SceneCB) + shadow
    float3 L = normalize(SunDirection.xyz);
    float3 LightColor = SunColor.rgb;
    float diffuseFactor = max(dot(worldNormal, L), 0.0);
    float4 viewPos = mul(float4(input.worldPos, 1.0), View);
    float shadowFactor = SampleShadowPCF(input.worldPos, viewPos.z);
    float3 directLight = albedo.rgb * diffuseFactor * LightColor * shadowFactor;

    // 事前準備: 視線・F0・フレネル
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float NdotV = max(dot(worldNormal, V), 0.0001);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);
    float3 F = F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);

    // 5. Ambient (Diffuse IBL) — Irradiance を法線方向でサンプル、エネルギー保存則で kD
    float3 irradiance = _IrradianceMap.Sample(smp, worldNormal).rgb;
    float3 kD = 1.0 - F;
    kD *= (1.0 - metallic);
    float3 ambientLight = irradiance * albedo.rgb * kD;

    // 6. Reflection (Specular IBL) — Split Sum: PrefilterEnv * (F0*A + B)
    float3 R = reflect(-V, worldNormal);
    const float PREFILTER_MIP_COUNT = 5.0;
    float mip = roughness * (PREFILTER_MIP_COUNT - 1.0);
    float3 prefiltered = _PrefilterEnv.SampleLevel(smp, R, mip).rgb;
    float2 brdf = _BrdfLut.Sample(smp, float2(NdotV, roughness)).rg;
    float3 specularPart = prefiltered * (F0 * brdf.x + brdf.y);

    // 7. Composite
    float3 finalColor = directLight + ambientLight + specularPart;

    finalColor += _RampTex.Sample(smp, float2(0.5f, 0.5f)).rgb * 0.0f;
    finalColor += (input.nprPerMesh.x + NprTuning2.x) * 0.0;

    return float4(finalColor, albedo.a);
}
