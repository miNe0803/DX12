// ============================================================
// Snow Particle Render Pixel Shader
// Soft circular snow flakes with depth fade.
// ============================================================

struct PSInput
{
    float4 svpos     : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float  alpha     : TEXCOORD1;
    float  viewDepth : TEXCOORD2;
};

float4 main(PSInput input) : SV_TARGET
{
    // Soft circular shape
    float dist = length(input.uv - 0.5) * 2.0;
    float circle = smoothstep(1.0, 0.5, dist);

    // Depth fade: near-camera particles are subtle
    float depthFade = saturate(input.viewDepth / 3.0);

    float finalAlpha = circle * input.alpha * depthFade * 0.65;

    // Discard invisible pixels
    if (finalAlpha < 0.005) discard;

    // Snow color: slightly blue-tinted white
    float3 snowColor = float3(0.92, 0.94, 1.0);

    return float4(snowColor * finalAlpha, finalAlpha); // premultiplied alpha
}
