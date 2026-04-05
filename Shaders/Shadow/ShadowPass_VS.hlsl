// Shadow depth-only VS: transforms vertices by World then LightVP.
// Per-draw: b0 holds the concatenated WorldLightVP matrix.
cbuffer ShadowDrawCB : register(b0)
{
    matrix WorldLightVP;
};

struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
};

float4 vert(VSInput input) : SV_POSITION
{
    return mul(float4(input.pos, 1.0f), WorldLightVP);
}
