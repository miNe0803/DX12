// --- [VS output] SimpleVS と同じ ---
struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
};

SamplerState smp : register(s0);
Texture2D _TreeMask   : register(t0); // R/G/B = 3種の木
Texture2D _NatureMask : register(t1); // R=雪, G=川, B=予備(インヒビタ等)
TextureCube _PrefilterEnv  : register(t4);
TextureCube _IrradianceMap : register(t5);
Texture2D _BrdfLut         : register(t6);

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[6]; // 0=地面, 1..3=木3種, 4=雪, 5=川
    float4 CameraPos;
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
    float3 treeMask = saturate(_TreeMask.Sample(smp, uv).rgb);
    float3 natureMask = saturate(_NatureMask.Sample(smp, uv).rgb);

    float treeWeight = saturate(treeMask.r + treeMask.g + treeMask.b);
    float snowWeight = natureMask.r;
    float riverWeight = natureMask.g;
    float inhibit = natureMask.b;

    float3 ground = LayerColor[0].rgb;
    float3 treeBlend = BlendTreeLayers(treeMask, ground, LayerColor[1].rgb, LayerColor[2].rgb, LayerColor[3].rgb);

    float3 albedo = ground;
    albedo = lerp(albedo, treeBlend, treeWeight);
    albedo = lerp(albedo, LayerColor[5].rgb, riverWeight);
    albedo = lerp(albedo, LayerColor[4].rgb, snowWeight);
    albedo *= (1.0 - saturate(inhibit));

    float roughness = 0.92;
    roughness = lerp(roughness, 0.80, treeWeight);
    roughness = lerp(roughness, 0.18, riverWeight);
    roughness = lerp(roughness, 0.45, snowWeight);
    roughness = clamp(roughness, 0.04, 1.0);
    float metallic = 0.0;

    float3 N = normalize(input.normal);
    float3 L = normalize(float3(0.5, 0.7, -1.0));
    float3 LightColor = float3(1.2, 1.2, 1.2);
    float diffuseFactor = max(dot(N, L), 0.0);
    float3 directLight = albedo * diffuseFactor * LightColor;

    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float NdotV = max(dot(N, V), 0.0001);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);

    float3 irradiance = _IrradianceMap.Sample(smp, N).rgb;
    float3 kD = 1.0 - F;
    kD *= (1.0 - metallic);
    float3 ambientLight = irradiance * albedo * kD;

    float3 R = reflect(-V, N);
    const float PREFILTER_MIP_COUNT = 5.0;
    float mip = roughness * (PREFILTER_MIP_COUNT - 1.0);
    float3 prefiltered = _PrefilterEnv.SampleLevel(smp, R, mip).rgb;
    float2 brdf = _BrdfLut.Sample(smp, float2(NdotV, roughness)).rg;
    float3 specularPart = prefiltered * (F0 * brdf.x + brdf.y);

    float3 finalColor = directLight + ambientLight + specularPart;
    return float4(finalColor, 1.0);
}
