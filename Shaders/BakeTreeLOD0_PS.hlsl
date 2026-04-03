// LOD0 ベイク用: 既存 PBR と同じルート（t0..t8 + b1）を満たし、単色 RT に焼き込む。
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
    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2);
    // アトラスは UNORM へ保存（アルファは葉の切り抜き用に保持）
    return float4(albedo.rgb, albedo.a);
}
