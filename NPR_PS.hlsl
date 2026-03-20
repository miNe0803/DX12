// NPR opaque: マニュアル準拠（固定影色・リム・露出）。ランプは NdotL ベースの u（t2 は PBR ではメタリックのため既定では未使用）
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
Texture2D _AlbedoMap : register(t0, space0);
Texture2D _NormalMap : register(t1, space0);
Texture2D _RampTex : register(t2, space0);
Texture2D _RoughnessMap : register(t3, space0);
TextureCube _PrefilterEnv  : register(t4, space0);
TextureCube _IrradianceMap : register(t5, space0);
Texture2D _BrdfLut         : register(t6, space0);

cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams;
    float4 CameraPos;
};

float4 main(VSOutput input) : SV_TARGET
{
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    clip(albedo.a - 0.5f);

    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2f);

    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    float3 decodedNormal = _NormalMap.Sample(smp, input.uv).rgb * 2.0f - 1.0f;
    float nScale = (RimParams.y > 0.001f) ? RimParams.y : 1.0f;
    float3 normalTS = normalize(lerp(float3(0, 0, 1), decodedNormal, saturate(nScale)));
    float3 worldNormal = normalize(mul(normalTS, TBN));

    float3 L = normalize(float3(0.5f, 0.7f, -1.0f));
    float NdotL = saturate(dot(worldNormal, L));
    float u = smoothstep(0.01f, 0.99f, NdotL);
    // t2: マニュアルどおりランプ（PBR ではメタリック。横長グラデーション PNG を割り当てると意図通り）
    float3 rampColor = _RampTex.SampleLevel(smp, float2(u, 0.5f), 0).rgb;

    float3 ambientColor = float3(0.20f, 0.22f, 0.28f);
    float3 shadowColor = albedo.rgb * ambientColor;
    float3 lightColor = albedo.rgb;
    float3 baseColor = lerp(shadowColor, lightColor, rampColor.r);

    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float rimPower = (RimParams.z > 0.001f) ? RimParams.z : 3.0f;
    float rim = pow(1.0f - saturate(dot(worldNormal, V)), rimPower);
    baseColor += rim * float3(1.0f, 1.0f, 1.0f) * 0.3f;

    const float nprExposure = 0.5f;
    float3 finalColor = baseColor * nprExposure;

    return float4(finalColor, albedo.a);
}
