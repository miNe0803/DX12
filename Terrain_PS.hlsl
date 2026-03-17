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
// t0-t7: 地形マスク (0=Snow_Snow, 1=Trees2_Trees, 2=Rivers_Rivers, 3=WaterColor_Out, 4=INHIBITORS_Out, 5=Snow_Depth, 6=Trees2_FreshWater, 7=Rivers_Depth)
Texture2D _Mask0 : register(t0);
Texture2D _Mask1 : register(t1);
Texture2D _Mask2 : register(t2);
Texture2D _Mask3 : register(t3);
Texture2D _Mask4 : register(t4);
Texture2D _Mask5 : register(t5);
Texture2D _Mask6 : register(t6);
Texture2D _Mask7 : register(t7);
TextureCube _PrefilterEnv  : register(t8);
TextureCube _IrradianceMap : register(t9);
Texture2D _BrdfLut         : register(t10);

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[4]; // 0=地面, 1=雪, 2=水, 3=木
    float4 CameraPos;
};

float4 main(VSOutput input) : SV_TARGET
{
    float2 uv = input.uv;
    float s = _Mask0.Sample(smp, uv).r;   // Snow_Snow
    float t = _Mask1.Sample(smp, uv).r;   // Trees2_Trees
    float r = _Mask2.Sample(smp, uv).r;   // Rivers_Rivers
    float w = _Mask3.Sample(smp, uv).r;   // WaterColor_Out
    float inhib = _Mask4.Sample(smp, uv).r; // INHIBITORS_Out

    float3 albedo = LayerColor[0].rgb; // 地面
    albedo = lerp(albedo, LayerColor[1].rgb, s);  // 雪
    albedo = lerp(albedo, LayerColor[2].rgb, max(r, w)); // 水（河川 or 水面）
    albedo = lerp(albedo, LayerColor[3].rgb, t);  // 木
    albedo *= (1.0 - saturate(inhib)); // INHIBITORS でマスクアウト

    float metallic = 0.0;
    float roughness = 0.85;
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
