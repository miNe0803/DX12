// ============================================================
// Water Reflection Miss Shader
// Returns sky color when reflection ray doesn't hit geometry.
// ============================================================

struct RayPayload
{
    float3 color;
    float  hitT;
};

[shader("miss")]
void Miss(inout RayPayload payload)
{
    // Sky gradient based on ray direction
    float3 dir = normalize(WorldRayDirection());
    float t = dir.y * 0.5 + 0.5; // 0=horizon, 1=zenith

    float3 horizonColor = float3(0.7, 0.8, 0.95);
    float3 zenithColor  = float3(0.3, 0.5, 0.9);
    payload.color = lerp(horizonColor, zenithColor, saturate(t));
    payload.hitT = -1.0; // no hit
}
