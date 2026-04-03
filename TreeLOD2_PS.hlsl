// ツリー遠景 LOD: アルベドのみ、法線は幾何の normal、ラフネス定数、clip なし（不透明扱い）
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

cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams;
    float4 CameraPos;
    float4 NprTuning;
    float4 NprTuning2;
    float4 NprDebugHdr;
};

static const float kTreeLod2Rough = 0.72f;

float4 main(VSOutput input) : SV_TARGET
{
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    albedo *= input.color;
    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2);

    float metallic = 0.0f;
    float roughness = kTreeLod2Rough;
    float3 worldNormal = normalize(input.normal);

    float3 L = normalize(float3(0.5, 0.7, -1.0));
    float3 LightColor = float3(1.2, 1.2, 1.2);
    float diffuseFactor = max(dot(worldNormal, L), 0.0);
    float3 directLight = albedo.rgb * diffuseFactor * LightColor;

    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float NdotV = max(dot(worldNormal, V), 0.0001);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);
    float3 F = F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);

    float3 irradiance = _IrradianceMap.Sample(smp, worldNormal).rgb;
    float3 kD = 1.0 - F;
    kD *= (1.0 - metallic);
    float3 ambientLight = irradiance * albedo.rgb * kD;

    float3 R = reflect(-V, worldNormal);
    const float PREFILTER_MIP_COUNT = 5.0;
    float mip = roughness * (PREFILTER_MIP_COUNT - 1.0);
    float3 prefiltered = _PrefilterEnv.SampleLevel(smp, R, mip).rgb;
    float2 brdf = _BrdfLut.Sample(smp, float2(NdotV, roughness)).rg;
    float3 specularPart = prefiltered * (F0 * brdf.x + brdf.y);

    float3 finalColor = directLight + ambientLight + specularPart;
    finalColor += _NormalMap.Sample(smp, input.uv).r * 0.0f;
    finalColor += _MetallicMap.Sample(smp, input.uv).r * 0.0f;
    finalColor += _RoughnessMap.Sample(smp, input.uv).r * 0.0f;
    finalColor += _RampTex.Sample(smp, float2(0.5f, 0.5f)).rgb * 0.0f;
    finalColor += _SphereMap.Sample(smp, input.uv).rgb * 0.0f;
    return float4(finalColor, 1.0);
}
