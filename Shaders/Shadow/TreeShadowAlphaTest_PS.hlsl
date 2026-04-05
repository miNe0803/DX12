// Alpha-test pixel shader for tree shadow map.
// Discards transparent leaf pixels so shadows don't include leaf-card quads.
// Uses the metallic map slot which stores the leaf alpha mask for trees.

Texture2D<float4> _AlphaMap : register(t0, space0);
SamplerState smp : register(s0);

struct PSInput
{
    float4 svpos : SV_POSITION;
    float2 uv : TEXCOORD;
};

void main(PSInput input)
{
    float alpha = _AlphaMap.Sample(smp, input.uv).r;
    clip(alpha - 0.3);
}
