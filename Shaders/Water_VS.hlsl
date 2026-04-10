// ============================================================
// Water Vertex Shader
// Renders water surface quads at a fixed Y height.
// Uses the terrain grid vertices with Y replaced by water surface level.
// ============================================================

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD;
    float3 Tangent  : TANGENT;
    float4 Color    : COLOR;
    uint4  BoneIdx  : BLENDINDICES;
    float4 BoneWt   : BLENDWEIGHT;
};

struct VSOutput
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 normal   : NORMAL;
};

cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
};

cbuffer WaterCB : register(b1)
{
    float4 WaterParams; // x=time, y=waterSurfaceY, z=foamWidth, w=specPower
};

VSOutput main(VSInput input)
{
    VSOutput output;

    // Replace Y with water surface height
    float3 worldPos = input.Position;
    worldPos.y = WaterParams.y;

    // Simple wave displacement (subtle vertex animation)
    float time = WaterParams.x;
    float waveHeight = 0.15;
    worldPos.y += sin(worldPos.x * 0.5 + time * 1.2) * waveHeight * 0.5;
    worldPos.y += sin(worldPos.z * 0.3 + time * 0.8) * waveHeight * 0.3;
    worldPos.y += cos((worldPos.x + worldPos.z) * 0.4 + time * 1.5) * waveHeight * 0.2;

    output.worldPos = worldPos;
    output.uv = input.UV;
    output.normal = float3(0, 1, 0); // water surface normal (up)

    matrix viewProj = mul(View, Proj);
    output.svpos = mul(float4(worldPos, 1.0), viewProj);

    return output;
}
