// Shadow-pass frustum culling for GPU-driven tree instances.
// Reuses TreeInfo from TreeFrustumHiZCull_CS but only tests against light frustum (no Hi-Z, no LOD).
// Outputs compacted visible-instance indices + indirect draw args for ExecuteIndirect.
//
// Root (CS):
//   b0: ShadowCullCB
//   Descriptor table: t0 (TreeInfo SRV), u0 (VisibleIndex UAV), u1 (IndirectArgs UAV)

cbuffer ShadowCullCB : register(b0)
{
    float4 FrustumPlanes[6]; // light-space frustum planes (Gribb-Hartmann, same convention as tree cull)
    float4 Params;           // x: instanceCount, y: maxOutput
    float4 CameraPos;        // xyz: camera world position, w: maxShadowDistance (XZ)
};

struct TreeInfo
{
    float4 centerRadius;
    row_major float4x4 worldRow;
    uint2 instanceGpuVA;
    uint speciesIndex;
    uint _pad0;
    uint2 _pad64_a;
    uint2 _pad64_b;
};

StructuredBuffer<TreeInfo>   gTrees        : register(t0);
RWStructuredBuffer<uint>     gVisibleIndex : register(u0);
RWByteAddressBuffer          gIndirectArgs : register(u1);

bool SphereOutsidePlane(float3 c, float r, float4 pl)
{
    float3 n = pl.xyz;
    float len = length(n);
    if (len < 1e-5)
        return false;
    float d = dot(n, c) + pl.w;
    return (d > r * len);
}

bool IsCulled(float3 c, float r)
{
    [unroll]
    for (int p = 0; p < 6; ++p)
    {
        if (SphereOutsidePlane(c, r, FrustumPlanes[p]))
            return true;
    }
    return false;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint id = dtid.x;
    uint n = (uint)Params.x;
    if (id >= n)
        return;

    TreeInfo t = gTrees[id];
    float3 c = t.centerRadius.xyz;
    float r = t.centerRadius.w;

    // Camera distance culling (XZ plane): skip trees far from camera
    float maxDist = CameraPos.w;
    if (maxDist > 0.0)
    {
        float2 dXZ = CameraPos.xz - c.xz;
        if (dot(dXZ, dXZ) > maxDist * maxDist)
            return;
    }

    // Frustum test (skip with Params.z != 0 for debugging)
    if (Params.z < 0.5 && IsCulled(c, r))
        return;

    uint maxOut = (uint)Params.y;
    uint slot;
    gIndirectArgs.InterlockedAdd(4, 1, slot);

    // Bounded allocation: roll back if we exceed max output
    if (slot >= maxOut)
    {
        uint unused;
        gIndirectArgs.InterlockedAdd(4, 0xFFFFFFFFu, unused); // decrement
        return;
    }

    gVisibleIndex[slot] = id;
}
