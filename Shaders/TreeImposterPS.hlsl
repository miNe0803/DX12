// LOD1 インポスター: 8 スライス横アトラス（BakeTreeLOD0 と同じレイアウト）
cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
};

struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
    float4 nprPerMesh : TEXCOORD2;
    float sliceU : TEXCOORD3;
    nointerpolation float3 treeBase : TEXCOORD4; // 足元（VS のビルボード足元＝ベイクの足元原点と一致）
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
    // 方位は足元アンカー基準（円筒ビルボードの軸とベイク時のカメラ軌道が一致）
    float3 toCam = CameraWorld.xyz - input.treeBase;
    toCam.y = 0.0f;
    float lenL = length(toCam);
    if (lenL < 1e-4f)
        toCam = float3(0, 0, 1);
    else
        toCam /= lenL;
    float ang = atan2(toCam.x, toCam.z);
    float uAngle = (ang + 3.14159265f) / (2.0f * 3.14159265f);
    float sliceF = uAngle * 8.0f;
    sliceF = min(sliceF, 7.999f);
    float s0 = floor(sliceF);
    float s1 = (s0 >= 7.5f) ? 0.0f : (s0 + 1.0f);
    float f = frac(sliceF);
    float hx = input.sliceU;
    float u0 = (s0 + hx) / 8.0f;
    float u1 = (s1 + hx) / 8.0f;
    float v = input.uv.y;
    float4 c0 = _AlbedoMap.SampleLevel(smp, float2(u0, v), 0);
    float4 c1 = _AlbedoMap.SampleLevel(smp, float2(u1, v), 0);
    float4 albedo = lerp(c0, c1, f);
    albedo *= input.color;
    clip(albedo.a - 0.12);
    return float4(albedo.rgb, 1.0);
}
