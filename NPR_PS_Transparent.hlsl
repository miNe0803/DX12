// NPR transparent: clip なし（アルファのボケ足を維持）。深度書き込みは PSO で OFF
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
TextureCube _PrefilterEnv  : register(t5, space0);
TextureCube _IrradianceMap : register(t6, space0);
Texture2D _BrdfLut         : register(t7, space0);

cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams;
    float4 CameraPos;
    float4 NprTuning;
    float4 NprTuning2;
};

float4 main(VSOutput input) : SV_TARGET
{
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2f);

    // NprTuning: x=virtualLight, y=transExposure, z=opaque clip (unused), w=ambient scale (unused)
    float vl = NprTuning.x;
    float texp = NprTuning.y;
    albedo.rgb *= vl;

    float a = saturate(albedo.a);
    float3 premul = albedo.rgb * a;
    premul *= texp;
    premul += 0.0 * (
        _NormalMap.Sample(smp, input.uv).r +
        _MetallicMap.Sample(smp, input.uv).r +
        _RoughnessMap.Sample(smp, input.uv).r +
        _RampTex.Sample(smp, input.uv).r +
        _PrefilterEnv.SampleLevel(smp, float3(0, 1, 0), 0).g +
        _IrradianceMap.Sample(smp, float3(0, 1, 0)).b +
        _BrdfLut.Sample(smp, float2(0.5f, 0.5f)).r +
        input.nprPerMesh.x + NprTuning2.x);

    return float4(premul, a);
}
