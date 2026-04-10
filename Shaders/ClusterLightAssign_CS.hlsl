// ============================================================
// Cluster Light Assignment Compute Shader
// Dispatched as Dispatch(tileCountX, tileCountY, depthSlices)
// Each thread group = one cluster.
// Tests all lights against the cluster AABB, writes matches.
// ============================================================

#include "ClusterShading.hlsli"

cbuffer ClusterAssignCB : register(b0)
{
    matrix InvProj;   // row_major, transposed from C++
    matrix ViewMat;   // row_major, transposed from C++
    float  nearZ;
    float  farZ;
    float  logScale;
    float  logBias;
    uint   tileCountX;
    uint   tileCountY;
    uint   depthSlices;
    uint   lightCount;
    uint   screenW;
    uint   screenH;
    uint2  _pad;
};

StructuredBuffer<LightData>  g_Lights       : register(t0);
RWStructuredBuffer<uint2>    g_ClusterData  : register(u0); // per-cluster (offset, count)
RWBuffer<uint>               g_LightIndices : register(u1); // global light index list
RWByteAddressBuffer          g_GlobalCounter: register(u2); // atomic append counter

// Reconstruct the view-space AABB of a cluster from its tile and depth slice.
void ComputeClusterAABB(uint3 clusterId, out float3 aabbMin, out float3 aabbMax)
{
    // Screen-space bounds of the tile
    float2 tileMin = float2(clusterId.x * CLUSTER_TILE_SIZE, clusterId.y * CLUSTER_TILE_SIZE);
    float2 tileMax = float2(tileMin.x + CLUSTER_TILE_SIZE, tileMin.y + CLUSTER_TILE_SIZE);

    // Normalize to [0, 1]
    float2 uvMin = tileMin / float2(screenW, screenH);
    float2 uvMax = tileMax / float2(screenW, screenH);

    // To NDC [-1, 1]
    float2 ndcMin = uvMin * 2.0 - 1.0;
    float2 ndcMax = uvMax * 2.0 - 1.0;
    ndcMin.y = -ndcMin.y;
    ndcMax.y = -ndcMax.y;
    // Swap Y since we flipped
    float tmpY = ndcMin.y;
    ndcMin.y = ndcMax.y;
    ndcMax.y = tmpY;

    // Depth slice boundaries (logarithmic)
    float sliceNear = nearZ * pow(farZ / nearZ, (float)clusterId.z / (float)depthSlices);
    float sliceFar  = nearZ * pow(farZ / nearZ, (float)(clusterId.z + 1) / (float)depthSlices);

    // Unproject 4 corners at near and far depth to view space
    // Using the inverse projection matrix
    float4 cornersNDC[4] = {
        float4(ndcMin.x, ndcMin.y, 0.0, 1.0),
        float4(ndcMax.x, ndcMin.y, 0.0, 1.0),
        float4(ndcMin.x, ndcMax.y, 0.0, 1.0),
        float4(ndcMax.x, ndcMax.y, 0.0, 1.0)
    };

    aabbMin = float3(1e10, 1e10, sliceNear);
    aabbMax = float3(-1e10, -1e10, sliceFar);

    [unroll] for (int i = 0; i < 4; ++i)
    {
        float4 viewCorner = mul(cornersNDC[i], InvProj);
        viewCorner /= viewCorner.w;

        // Scale to near and far depth
        float3 dirView = viewCorner.xyz / viewCorner.z;
        float3 pNear = dirView * sliceNear;
        float3 pFar  = dirView * sliceFar;

        aabbMin.xy = min(aabbMin.xy, min(pNear.xy, pFar.xy));
        aabbMax.xy = max(aabbMax.xy, max(pNear.xy, pFar.xy));
    }
}

bool SphereAABBIntersect(float3 center, float radius, float3 aabbMin, float3 aabbMax)
{
    float3 closest = clamp(center, aabbMin, aabbMax);
    float3 diff = closest - center;
    return dot(diff, diff) <= (radius * radius);
}

groupshared uint s_lightCount;
groupshared uint s_lightList[MAX_LIGHTS_PER_CLUSTER];
groupshared uint s_globalOffset;

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    uint clusterIdx = gid.z * (tileCountX * tileCountY) + gid.y * tileCountX + gid.x;

    if (gtid == 0) s_lightCount = 0;
    GroupMemoryBarrierWithGroupSync();

    // Compute cluster AABB in view space
    float3 aabbMin, aabbMax;
    ComputeClusterAABB(gid, aabbMin, aabbMax);

    // Each thread tests a subset of lights
    for (uint i = gtid; i < lightCount; i += 64)
    {
        LightData light = g_Lights[i];

        // Transform light position to view space
        float3 lightPosView = mul(float4(light.position, 1.0), ViewMat).xyz;

        if (SphereAABBIntersect(lightPosView, light.range, aabbMin, aabbMax))
        {
            uint slot;
            InterlockedAdd(s_lightCount, 1, slot);
            if (slot < MAX_LIGHTS_PER_CLUSTER)
                s_lightList[slot] = i;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Allocate global space for this cluster's lights
    uint localCount = min(s_lightCount, MAX_LIGHTS_PER_CLUSTER);
    if (gtid == 0 && localCount > 0)
    {
        g_GlobalCounter.InterlockedAdd(0, localCount, s_globalOffset);
    }

    GroupMemoryBarrierWithGroupSync();

    // Write cluster data
    if (gtid == 0)
    {
        g_ClusterData[clusterIdx] = uint2(localCount > 0 ? s_globalOffset : 0, localCount);
    }

    // Write light indices to global list
    for (uint j = gtid; j < localCount; j += 64)
    {
        g_LightIndices[s_globalOffset + j] = s_lightList[j];
    }
}
