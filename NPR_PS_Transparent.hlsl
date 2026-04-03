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

float4 main(VSOutput input) : SV_TARGET
{
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    albedo *= input.color;
    albedo.a *= saturate(input.nprPerMesh.z);
    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2f);

    int sphMode = (int)floor(input.nprPerMesh.y + 0.5f);
    if (sphMode > 0)
    {
        float4 sph = _SphereMap.Sample(smp, input.uv);
        sph.rgb = pow(max(sph.rgb, 1e-5), 2.2f);
        if (sphMode == 1)
            albedo.rgb *= sph.rgb;
        else
            albedo.rgb += sph.rgb;
    }

    // NprTuning: x=virtualLight, y=未使用, z/w=不透明パス用
    float vl = NprTuning.x;
    albedo.rgb *= vl;

    float a = saturate(albedo.a);
    float3 premul = albedo.rgb * a;
    premul += 0.0 * (
        _NormalMap.Sample(smp, input.uv).r +
        _MetallicMap.Sample(smp, input.uv).r +
        _RoughnessMap.Sample(smp, input.uv).r +
        _RampTex.Sample(smp, input.uv).r +
        _PrefilterEnv.SampleLevel(smp, float3(0, 1, 0), 0).g +
        _IrradianceMap.Sample(smp, float3(0, 1, 0)).b +
        _BrdfLut.Sample(smp, float2(0.5f, 0.5f)).r +
        input.nprPerMesh.x + NprTuning2.x + NprTuning.x);

    return float4(premul, a);
}
