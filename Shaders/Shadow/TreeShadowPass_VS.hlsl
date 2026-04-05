// Shadow depth-only VS for GPU-driven trees.
// Reads per-instance world matrix from StructuredBuffer and projects with LightVP.
// No visible-index indirection: all uploaded instances are drawn.
// Root:
//   b0: LightVP (single cascade matrix)
//   t0 space1: StructuredBuffer<InstanceData>

cbuffer ShadowCascadeCB : register(b0)
{
    matrix LightVP;
};

struct InstanceData
{
    matrix World;
    float4 NprPerMesh;
};

StructuredBuffer<InstanceData> gInstanceData : register(t0, space1);

struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
};

struct VSOutput
{
    float4 svpos : SV_POSITION;
    float2 uv : TEXCOORD;
};

VSOutput vert(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    matrix worldM = gInstanceData[instanceID].World;
    float4 worldPos = mul(float4(input.pos, 1.0f), worldM);
    output.svpos = mul(worldPos, LightVP);
    output.uv = input.uv;
    return output;
}
