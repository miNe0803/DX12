// ============================================================
// Snow Particle Render Vertex Shader
// DrawInstanced(4, particleCount): 4 verts per billboard quad.
// No geometry shader needed — billboard expansion in VS.
// ============================================================

struct SnowParticle
{
    float3 position;
    float  lifetime;
    float3 velocity;
    float  size;
};

struct VSOutput
{
    float4 svpos     : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float  alpha     : TEXCOORD1;
    float  viewDepth : TEXCOORD2;
};

cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
};

StructuredBuffer<SnowParticle> g_Particles : register(t0);

// Quad vertex offsets (4 vertices, triangle strip order)
static const float2 kQuadOffsets[4] = {
    float2(-0.5, -0.5), // bottom-left
    float2( 0.5, -0.5), // bottom-right
    float2(-0.5,  0.5), // top-left
    float2( 0.5,  0.5), // top-right
};

static const float2 kQuadUVs[4] = {
    float2(0, 1),
    float2(1, 1),
    float2(0, 0),
    float2(1, 0),
};

VSOutput main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOutput output;

    SnowParticle p = g_Particles[instanceID];

    // Billboard: expand quad in view space
    float3 worldPos = p.position;
    float4 viewPos = mul(float4(worldPos, 1.0), View);

    float halfSize = p.size * 0.5;
    float2 offset = kQuadOffsets[vertexID] * halfSize;
    viewPos.xy += offset;

    output.svpos = mul(viewPos, Proj);
    output.uv = kQuadUVs[vertexID];
    output.viewDepth = viewPos.z;

    // Fade based on lifetime (fade in at spawn, fade out at death)
    float fadeIn  = saturate(p.lifetime / 0.5);  // fade in over 0.5s
    float fadeOut = saturate((8.0 - p.lifetime) / 2.0); // assume max ~8s
    output.alpha = fadeIn * (1.0 - saturate(1.0 - p.lifetime / 1.0));

    // Dead particles: zero alpha
    if (p.lifetime <= 0.0) output.alpha = 0.0;

    return output;
}
