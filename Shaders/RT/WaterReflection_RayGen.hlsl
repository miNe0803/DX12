// ============================================================
// Water Reflection Ray Generation Shader
// Dispatched at half resolution. For each pixel, check if it
// corresponds to a water surface, then trace a reflection ray.
// ============================================================

RaytracingAccelerationStructure g_TLAS : register(t0);
RWTexture2D<float4> g_Output : register(u0);
Texture2D<float> g_Depth : register(t1);

cbuffer RTCB : register(b0)
{
    matrix InvViewProj;
    float4 CameraPos;
    float  waterSurfaceY;
    uint   outputWidth;
    uint   outputHeight;
    uint   _pad;
};

SamplerState g_PointClamp : register(s0);

struct RayPayload
{
    float3 color;
    float  hitT;
};

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clip = float4(ndc, depth, 1.0);
    float4 world = mul(clip, InvViewProj);
    return world.xyz / world.w;
}

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIdx = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // Map to full-resolution UV (half-res dispatch)
    float2 uv = (float2(launchIdx) + 0.5) / float2(launchDim);

    // Sample depth at full resolution UV
    float depth = g_Depth.SampleLevel(g_PointClamp, uv, 0);

    // Reconstruct world position
    float3 worldPos = ReconstructWorldPos(uv, depth);

    // Only trace for pixels near water surface (within threshold)
    float waterThreshold = 2.0; // meters tolerance
    if (abs(worldPos.y - waterSurfaceY) > waterThreshold)
    {
        g_Output[launchIdx] = float4(0, 0, 0, 0);
        return;
    }

    // Compute reflection direction
    float3 viewDir = normalize(worldPos - CameraPos.xyz);
    float3 waterNormal = float3(0, 1, 0); // simplified; could sample water normal map
    float3 reflectDir = reflect(viewDir, waterNormal);

    // Trace reflection ray
    RayDesc ray;
    ray.Origin = worldPos + waterNormal * 0.1; // offset to avoid self-intersection
    ray.Direction = reflectDir;
    ray.TMin = 0.01;
    ray.TMax = 1000.0;

    RayPayload payload;
    payload.color = float3(0, 0, 0);
    payload.hitT = -1.0;

    TraceRay(g_TLAS,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF,           // instance mask
        0,              // hit group index
        0,              // multiplier
        0,              // miss shader index
        ray,
        payload);

    g_Output[launchIdx] = float4(payload.color, payload.hitT > 0 ? 1.0 : 0.0);
}
