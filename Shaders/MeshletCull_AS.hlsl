// ============================================================
// Amplification Shader: per-meshlet culling
// Dispatched as DispatchMesh(ceil(meshletCount/32), 1, 1)
// Each thread group processes up to 32 meshlets.
// ============================================================

#include "Bindless.hlsli"

// Scene constants
cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection;
    float4 SunColor;
    matrix InvViewProj;
    float4 ClusterGridParams;
    float4 ClusterSliceParams;
    uint   lightBufferSRVIdx;
    uint   clusterDataSRVIdx;
    uint   lightIndexListSRVIdx;
    uint   activeLightCount;
};

// Per-dispatch constants (root constants b2)
cbuffer DrawConstants : register(b2)
{
    uint g_MeshletBufferIdx;     // StructuredBuffer<Meshlet>
    uint g_BoundsBufferIdx;      // StructuredBuffer<MeshletBounds>
    uint g_MeshletCount;         // total meshlets to process
    uint g_InstanceWorldIdx;     // StructuredBuffer<InstanceData> for world matrix
};

struct MeshletData
{
    uint vertexOffset;
    uint vertexCount;
    uint primitiveOffset;
    uint primitiveCount;
};

struct BoundsData
{
    float3 center;
    float  radius;
    float3 coneAxis;
    float  coneCutoff;
};

// Frustum planes (extracted from ViewProj)
static float4 ExtractPlane(matrix vp, int row, float sign)
{
    return float4(
        vp[0][3] + sign * vp[0][row],
        vp[1][3] + sign * vp[1][row],
        vp[2][3] + sign * vp[2][row],
        vp[3][3] + sign * vp[3][row]);
}

bool SphereOutsidePlane(float3 center, float radius, float4 plane)
{
    float dist = dot(plane.xyz, center) + plane.w;
    return dist > radius * length(plane.xyz);
}

bool FrustumCullSphere(float3 center, float radius, matrix vp)
{
    float4 planes[6];
    planes[0] = ExtractPlane(vp, 0, 1.0);  // left
    planes[1] = ExtractPlane(vp, 0, -1.0); // right
    planes[2] = ExtractPlane(vp, 1, 1.0);  // bottom
    planes[3] = ExtractPlane(vp, 1, -1.0); // top
    planes[4] = ExtractPlane(vp, 2, 1.0);  // near
    planes[5] = ExtractPlane(vp, 2, -1.0); // far

    [unroll] for (int i = 0; i < 6; ++i)
    {
        if (SphereOutsidePlane(center, radius, planes[i]))
            return true; // culled
    }
    return false;
}

bool BackfaceConeCull(float3 coneAxis, float coneCutoff, float3 meshletCenter, float3 cameraPos)
{
    // If all normals in the meshlet face away from the camera, cull it.
    // coneCutoff: minimum dot(normal, avgNormal) across all normals in the meshlet.
    // If dot(viewDir, coneAxis) < -coneCutoff, the entire meshlet is backfacing.
    if (coneCutoff >= 1.0) return false; // degenerate cone, don't cull
    float3 viewDir = normalize(meshletCenter - cameraPos);
    return dot(viewDir, coneAxis) < -coneCutoff;
}

// Payload passed from AS to MS
struct MeshletPayload
{
    uint meshletIndices[32];
};

groupshared MeshletPayload s_payload;

[numthreads(32, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint dtid : SV_DispatchThreadID,
    uint gid  : SV_GroupID)
{
    bool visible = false;

    if (dtid < g_MeshletCount)
    {
        StructuredBuffer<BoundsData> boundsBuffer = ResourceDescriptorHeap[g_BoundsBufferIdx];
        BoundsData bounds = boundsBuffer[dtid];

        matrix viewProj = mul(View, Proj);

        // Frustum culling (world-space sphere — assumes identity instance transform for now)
        // TODO: transform bounds by instance world matrix for instanced meshlets
        visible = !FrustumCullSphere(bounds.center, bounds.radius, viewProj);

        // Backface cone culling
        if (visible)
        {
            visible = !BackfaceConeCull(bounds.coneAxis, bounds.coneCutoff,
                                        bounds.center, CameraWorld.xyz);
        }

        // Hi-Z occlusion culling placeholder (Step 3 will add Two-Phase here)
    }

    // Compact visible meshlets
    if (visible)
        s_payload.meshletIndices[gtid] = dtid;
    else
        s_payload.meshletIndices[gtid] = 0xFFFFFFFF;

    // Count visible meshlets in this group
    uint visibleCount = WaveActiveCountBits(visible);

    // Dispatch mesh shader groups for visible meshlets
    DispatchMesh(visibleCount, 1, 1, s_payload);
}
