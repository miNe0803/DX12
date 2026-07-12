// ============================================================
// Ocean Vertex Shader
// 地形メッシュとは独立した「巨大な水平面」。地平線まで水を広げるための専用パス。
// Water_VS は地形メッシュ依存で、地形端より外側には水が無い欠点を補う。
// 入力 VB は 4 頂点の超巨大クワッド (XZ ± 50km)。カメラに追従して常にカメラ周囲を覆う。
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

// 海面の絶対 Y 高度 (m)。地形掘削後の川底 (orig - 6m) より少し上、土手 (orig) より下を狙う。
// 川面と海面が滑らかにつながるよう、Water_VS の水位 (orig - carveAmount + 0.5m) と
// 概ね一致させるのが理想。シーンに応じて要調整。
static const float kOceanY = 2.0;

VSOutput main(VSInput input)
{
    VSOutput output;
    float time = CameraPos.w;

    // カメラを中心に巨大プレーンを配置 → 常に視界の地平線まで覆う
    float3 worldPos;
    worldPos.x = input.Position.x + CameraPos.x;
    worldPos.y = kOceanY;
    worldPos.z = input.Position.z + CameraPos.z;

    // 大きく緩い波 (海らしいうねり)
    worldPos.y += sin(worldPos.x * 0.05 + time * 1.1) * 0.10;
    worldPos.y += cos(worldPos.z * 0.04 + time * 0.7) * 0.08;

    output.worldPos = worldPos;
    // UV: ワールド XZ から計算 (タイル状の表面ノイズ用)
    output.uv = float2(worldPos.x * 0.001, worldPos.z * 0.001);
    output.normal = float3(0, 1, 0);

    float4 wp = float4(worldPos, 1.0);
    float4 viewPos = mul(wp, View);
    output.svpos = mul(viewPos, Proj);
    return output;
}
