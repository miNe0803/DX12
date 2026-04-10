#ifndef TWO_PHASE_OCCLUSION_HLSLI
#define TWO_PHASE_OCCLUSION_HLSLI

// ============================================================
// Two-Phase Occlusion Culling — shared Hi-Z test logic
//
// Phase 1: Test against PREVIOUS frame's Hi-Z.
//   - Visible → draw immediately
//   - Culled  → write to "culled list" for Phase 2
//
// Phase 2: After Phase 1 draws, rebuild Hi-Z from new depth.
//   Test ONLY Phase 1 culled items against NEW Hi-Z.
//   - Visible → draw (newly revealed objects)
//   - Culled  → truly occluded, skip
//
// This fixes "false occlusion elimination" because objects
// only stay culled if they fail BOTH tests.
// ============================================================

// Hi-Z pyramid (set externally before dispatch)
Texture2D<float> g_HiZPyramid : register(t10, space0);
SamplerState g_PointClampSampler : register(s2);

// Two-phase control
cbuffer TwoPhaseConstants : register(b3, space0)
{
    uint g_Phase;              // 0 = Phase1 (prev Hi-Z), 1 = Phase2 (new Hi-Z)
    uint g_HiZWidth;
    uint g_HiZHeight;
    uint g_HiZMipCount;
    float g_HiZNearDisableDist; // skip Hi-Z for nearby objects (meters)
    float g_HiZDepthBias;       // conservative bias
    float g_HiZMaxPixelRadius;  // skip Hi-Z if screen projection too large
    uint  g_TotalMeshlets;      // total meshlets being processed
};

// Phase 1 output / Phase 2 input: culled meshlet indices
RWByteAddressBuffer g_CulledList  : register(u0, space0);  // Phase 1 writes, Phase 2 reads
RWByteAddressBuffer g_CulledCount : register(u1, space0);  // atomic counter

/// Project a world-space sphere to screen and test against Hi-Z.
/// Returns true if the sphere is OCCLUDED (should be culled).
bool HiZOcclusionTest(float3 sphereCenter, float sphereRadius,
                      matrix viewProj, float3 cameraPos)
{
    // Skip Hi-Z for nearby objects (always visible)
    float distXZ = length(sphereCenter.xz - cameraPos.xz);
    if (distXZ < g_HiZNearDisableDist)
        return false; // not occluded

    // Project sphere center to clip space
    float4 clipCenter = mul(float4(sphereCenter, 1.0), viewProj);
    if (clipCenter.w <= 0.0)
        return false; // behind camera, frustum cull handles this

    float3 ndc = clipCenter.xyz / clipCenter.w;

    // Project sphere radius to screen pixels
    float4 clipEdge = mul(float4(sphereCenter + float3(sphereRadius, 0, 0), 1.0), viewProj);
    float ndcEdgeX = clipEdge.x / clipEdge.w;
    float screenRadius = abs(ndcEdgeX - ndc.x) * 0.5 * (float)g_HiZWidth;

    // Skip if projection is too large (likely a nearby large object)
    if (screenRadius > g_HiZMaxPixelRadius)
        return false;

    // Compute mip level from screen-space radius
    float mipF = max(0.0, log2(max(1.0, screenRadius)));
    uint mipLevel = min((uint)mipF, g_HiZMipCount - 1);

    // Sample Hi-Z at the projected center (UV space)
    float2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;

    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1)
        return false;

    // Sample 5 neighbors for robustness
    float mipW = max(1.0, (float)g_HiZWidth / (float)(1u << mipLevel));
    float mipH = max(1.0, (float)g_HiZHeight / (float)(1u << mipLevel));
    float2 texelSize = float2(1.0 / mipW, 1.0 / mipH);

    float maxDepth = g_HiZPyramid.SampleLevel(g_PointClampSampler, uv, (float)mipLevel).r;
    maxDepth = max(maxDepth, g_HiZPyramid.SampleLevel(g_PointClampSampler, uv + float2( texelSize.x, 0), (float)mipLevel).r);
    maxDepth = max(maxDepth, g_HiZPyramid.SampleLevel(g_PointClampSampler, uv + float2(-texelSize.x, 0), (float)mipLevel).r);
    maxDepth = max(maxDepth, g_HiZPyramid.SampleLevel(g_PointClampSampler, uv + float2(0,  texelSize.y), (float)mipLevel).r);
    maxDepth = max(maxDepth, g_HiZPyramid.SampleLevel(g_PointClampSampler, uv + float2(0, -texelSize.y), (float)mipLevel).r);

    // Forward-Z: object is occluded if its nearest depth > max depth in Hi-Z + bias
    // ndc.z is the sphere's nearest depth (center - radius projected)
    float sphereNearDepth = ndc.z - (sphereRadius / clipCenter.w) * 0.5;
    sphereNearDepth = saturate(sphereNearDepth);

    return sphereNearDepth > (maxDepth + g_HiZDepthBias);
}

/// Phase 1: write culled meshlet index to the culled list.
void WriteToCulledList(uint meshletIndex)
{
    uint slot;
    g_CulledCount.InterlockedAdd(0, 1, slot);
    g_CulledList.Store(slot * 4, meshletIndex);
}

/// Phase 2: read culled meshlet index from the culled list.
uint ReadFromCulledList(uint slotIndex)
{
    return g_CulledList.Load(slotIndex * 4);
}

#endif // TWO_PHASE_OCCLUSION_HLSLI
