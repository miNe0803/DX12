// ============================================================
// Water Reflection Closest Hit Shader
// Returns the albedo of the hit geometry for reflection.
// ============================================================

struct RayPayload
{
    float3 color;
    float  hitT;
};

struct HitAttributes
{
    float2 barycentrics;
};

// Simplified: return a basic color based on hit distance.
// In production, this would sample the hit geometry's albedo via bindless.
[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in HitAttributes attribs)
{
    // Hit distance for intensity falloff
    float t = RayTCurrent();
    payload.hitT = t;

    // Approximate color: green for foliage (trees), brown for ground
    // In full implementation, fetch material from bindless buffers using:
    //   InstanceID() → material index → albedo texture
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * t;

    // Simple height-based color approximation
    float heightFactor = saturate((hitPos.y - 0.0) / 50.0);
    float3 groundColor = float3(0.15, 0.12, 0.08);
    float3 foliageColor = float3(0.08, 0.20, 0.06);
    payload.color = lerp(groundColor, foliageColor, heightFactor);

    // Distance fog
    float fogFactor = saturate(t / 500.0);
    float3 fogColor = float3(0.6, 0.7, 0.85);
    payload.color = lerp(payload.color, fogColor, fogFactor * fogFactor);
}
