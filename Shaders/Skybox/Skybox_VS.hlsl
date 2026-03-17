// Skybox (correct infinite-sky approach):
// - Draw a fullscreen triangle
// - Reconstruct view ray per-pixel from inverse projection
// - Rotate into world using inverse view rotation (no translation)

cbuffer SkyboxCB : register(b0)
{
    matrix InvProj;
    matrix InvViewNoTrans;
};

struct PSInput
{
    float4 svpos : SV_POSITION;
    float2 ndc   : TEXCOORD0; // [-1,1]
};

PSInput main(uint vid : SV_VertexID)
{
    // Fullscreen triangle in clip-space
    float2 pos;
    if (vid == 0) pos = float2(-1.0, -1.0);
    else if (vid == 1) pos = float2(-1.0,  3.0);
    else pos = float2( 3.0, -1.0);

    PSInput o;
    o.ndc = pos;
    o.svpos = float4(pos, 1.0, 1.0); // z=w -> depth 1.0 (furthest)
    return o;
}
