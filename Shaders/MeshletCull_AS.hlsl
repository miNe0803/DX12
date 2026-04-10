// ============================================================
// Amplification Shader: per-meshlet culling with Two-Phase Occlusion
//
// Phase 0 (no Hi-Z): frustum + backface only (fallback)
// Phase 1: frustum + backface + previous-frame Hi-Z
//          Culled meshlets written to g_CulledList for Phase 2
// Phase 2: process ONLY Phase 1 culled items against new Hi-Z
// ============================================================

#include "Bindless.hlsli"
#include "TwoPhaseOcclusion.hlsli"

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

cbuffer DrawConstants : register(b2)
{
    uint g_MeshletBufferIdx;     // StructuredBuffer<Meshlet>
    uint g_BoundsBufferIdx;      // StructuredBuffer<MeshletBounds>
    uint g_MeshletCount;         // total meshlets to process (Phase 1) or culled count (Phase 2)
    uint g_InstanceWorldIdx;     // unused for now (identity instance)
};

struct BoundsData
{
    float3 center;
    float  radius;
    float3 coneAxis;
    float  coneCutoff;
};

// --- Frustum culling ---

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
    planes[0] = ExtractPlane(vp, 0,  1.0); // left
    planes[1] = ExtractPlane(vp, 0, -1.0); // right
    planes[2] = ExtractPlane(vp, 1,  1.0); // bottom
    planes[3] = ExtractPlane(vp, 1, -1.0); // top
    planes[4] = ExtractPlane(vp, 2,  1.0); // near
    planes[5] = ExtractPlane(vp, 2, -1.0); // far

    [unroll] for (int i = 0; i < 6; ++i)
    {
        if (SphereOutsidePlane(center, radius, planes[i]))
            return true;
    }
    return false;
}

bool BackfaceConeCull(float3 coneAxis, float coneCutoff, float3 center, float3 cameraPos)
{
    if (coneCutoff >= 1.0) return false;
    float3 viewDir = normalize(center - cameraPos);
    return dot(viewDir, coneAxis) < -coneCutoff;
}

// --- Payload to Mesh Shader ---

struct MeshletPayload
{
    uint meshletIndices[32];
};

groupshared MeshletPayload s_payload;
groupshared uint s_visibleCount;

[numthreads(32, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint dtid : SV_DispatchThreadID,
    uint gid  : SV_GroupID)
{
    if (gtid == 0) s_visibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    bool visible = false;
    uint meshletIdx = 0xFFFFFFFF;

    if (dtid < g_MeshletCount)
    {
        // Phase 2: read actual meshlet index from culled list
        if (g_Phase == 1)
            meshletIdx = ReadFromCulledList(dtid);
        else
            meshletIdx = dtid;

        if (meshletIdx != 0xFFFFFFFF && meshletIdx < g_TotalMeshlets)
        {
            StructuredBuffer<BoundsData> boundsBuffer = ResourceDescriptorHeap[g_BoundsBufferIdx];
            BoundsData bounds = boundsBuffer[meshletIdx];

            matrix viewProj = mul(View, Proj);

            // Frustum culling
            visible = !FrustumCullSphere(bounds.center, bounds.radius, viewProj);

            // Backface cone culling
            if (visible)
                visible = !BackfaceConeCull(bounds.coneAxis, bounds.coneCutoff,
                                            bounds.center, CameraWorld.xyz);

            // Hi-Z occlusion culling (Phase 1 uses prev Hi-Z, Phase 2 uses new Hi-Z)
            if (visible && g_HiZMipCount > 0)
            {
                bool occluded = HiZOcclusionTest(bounds.center, bounds.radius,
                                                  mul(View, Proj), CameraWorld.xyz);
                if (occluded)
                {
                    visible = false;

                    // Phase 1 only: save to culled list for Phase 2 re-test
                    if (g_Phase == 0)
                        WriteToCulledList(meshletIdx);
                }
            }
        }
    }

    // Compact visible meshlets using wave intrinsics
    if (visible)
    {
        uint slot;
        InterlockedAdd(s_visibleCount, 1, slot);
        s_payload.meshletIndices[slot] = meshletIdx;
    }

    GroupMemoryBarrierWithGroupSync();

    // Fill unused slots
    if (gtid >= s_visibleCount && gtid < 32)
        s_payload.meshletIndices[gtid] = 0xFFFFFFFF;

    DispatchMesh(s_visibleCount, 1, 1, s_payload);
}
