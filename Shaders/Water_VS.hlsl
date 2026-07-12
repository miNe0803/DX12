// ============================================================
// Water Vertex Shader (Terrain Root Signature 対応版)
// 元のテレイン高さ (川掘削前) を水面高さとして使用。
// Terrain VS は川マスクで Y を下げる -> 水面は掘削前の位置に残る
// -> 水面が川底より上に浮き、River ribbon として表示される。
// ============================================================

cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[6];
    float4 CameraPos;    // .w = time
    float4 DebugParams;
    float4 SunDirection;
    float4 SunColor;
};

SamplerState smp : register(s0);
Texture2D _TreeMaskVS   : register(t0);
Texture2D _NatureMaskVS : register(t1);
Texture2D _GroundDiffVS : register(t2);
Texture2D _GroundDispVS : register(t3);
Texture2D _RiversMaskVS : register(t4);
Texture2D _SnowMaskVS   : register(t5);

// TerrainVS と整合する 5x5+ ガウスぼかし
float SmoothRiverMaskVS(float2 uv)
{
    const float t = 1.0 / 512.0;
    float v = 0.0;
    v += _RiversMaskVS.SampleLevel(smp, uv, 0).r * 4.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( t,  0), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-t,  0), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0,  t), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0, -t), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( t,  t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-t,  t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( t, -t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-t, -t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 2*t, 0), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-2*t, 0), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0,  2*t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0, -2*t), 0).r;
    return v / 20.0;
}

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD;
    float3 Tangent  : TANGENT;
    float4 Color    : COLOR;
    uint4  BoneIdx  : BONEINDEX;
    float4 BoneWt   : BONEWEIGHT;
};

struct VSOutput
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 normal   : NORMAL;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float time = CameraPos.w;

    // TerrainVS と完全に同じ掘削量を計算 → その上 0.5m を水面に。
    // 旧式の dip 計算は TerrainVS と式が違ったため水面 < 地形 になり LESS_EQUAL 失敗の領域があった。
    // 同じ式にすることで、川全域で常に水面が地形より 0.5m 上にいることが保証される。
    static const float kRiverCarveDepth = 6.0; // TerrainVS と一致
    static const float kRiverCarveStart = 0.10; // TerrainVS と一致
    float riverSmooth = SmoothRiverMaskVS(input.UV);
    float carve = saturate((riverSmooth - kRiverCarveStart) / (1.0 - kRiverCarveStart));
    carve = carve * carve * (3.0 - 2.0 * carve); // TerrainVS と同じ smoothstep
    float carveAmount = carve * kRiverCarveDepth;

    float3 localPos = input.Position;
    localPos.y -= (carveAmount - 0.5); // 川底より 0.5m 上に水面 → 水深 ≒ 5.5m まで
    float4 worldPos = mul(float4(localPos, 1.0f), World);

    // 波による Y 変位 (軽微) - 振幅をかなり小さく
    float waveAmp = 0.04;
    worldPos.y += sin(worldPos.x * 0.35 + time * 1.3) * waveAmp;
    worldPos.y += cos(worldPos.z * 0.28 + time * 0.9) * waveAmp * 0.7;

    output.worldPos = worldPos.xyz;
    output.uv = input.UV;
    output.normal = float3(0, 1, 0);

    float4 viewPos = mul(worldPos, View);
    output.svpos = mul(viewPos, Proj);
    return output;
}
