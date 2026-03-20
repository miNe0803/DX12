// NPR transparent: アンリット寄せ + 乗算済みアルファ（目影・ハイライト用）。板ポリは PSO 側で Cull None 推奨
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
Texture2D _MetallicMap : register(t2, space0);
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
    clip(albedo.a - 0.001f);

    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2f);

    const float nprExposure = 0.8f;
    const float virtualLight = 0.85f;
    albedo.rgb *= virtualLight;

    float a = saturate(albedo.a);
    float3 premul = albedo.rgb * a;
    premul *= nprExposure;

    return float4(premul, a);
}
