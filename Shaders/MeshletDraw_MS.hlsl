// ============================================================
// Mesh Shader: fetch vertices from meshlet data, transform, output
// One thread group = one meshlet (max 64 verts, 124 prims).
// ============================================================

#include "Bindless.hlsli"

cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection;
    float4 SunColor;
    matrix InvViewProj;
};

cbuffer DrawConstants : register(b2)
{
    uint g_MeshletBufferIdx;       // StructuredBuffer<Meshlet>
    uint g_UniqueVertexIdxBufIdx;  // ByteAddressBuffer
    uint g_PrimitiveIdxBufIdx;     // ByteAddressBuffer (packed)
    uint g_VertexBufferIdx;        // StructuredBuffer<Vertex>
};

struct MeshletData
{
    uint vertexOffset;
    uint vertexCount;
    uint primitiveOffset;
    uint primitiveCount;
};

struct VertexData
{
    float3 Position;
    float3 Normal;
    float2 UV;
    float3 Tangent;
    float4 Color;
    uint4  BoneIndex;    // packed uint16x4 → loaded as 2 uint32
    float4 BoneWeight;
};

struct MeshletPayload
{
    uint meshletIndices[32];
};

struct MSOutput
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD1;
    float3 tangent  : TANGENT;
    float4 color    : COLOR;
    uint   materialIndex : TEXCOORD2;
};

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    in payload MeshletPayload payload,
    out vertices MSOutput verts[64],
    out indices uint3 tris[124])
{
    uint meshletIdx = payload.meshletIndices[gid];
    if (meshletIdx == 0xFFFFFFFF)
        return;

    StructuredBuffer<MeshletData> meshlets = ResourceDescriptorHeap[g_MeshletBufferIdx];
    MeshletData meshlet = meshlets[meshletIdx];

    SetMeshOutputCounts(meshlet.vertexCount, meshlet.primitiveCount);

    ByteAddressBuffer uniqueVertexIndices = ResourceDescriptorHeap[g_UniqueVertexIdxBufIdx];
    ByteAddressBuffer primitiveIndices    = ResourceDescriptorHeap[g_PrimitiveIdxBufIdx];

    // Load vertex buffer as raw structured data (84 bytes per vertex)
    // Using a structured buffer view registered with sizeof(Vertex) stride
    StructuredBuffer<VertexData> vertexBuffer = ResourceDescriptorHeap[g_VertexBufferIdx];

    matrix viewProj = mul(View, Proj);

    // --- Output vertices ---
    if (gtid < meshlet.vertexCount)
    {
        uint globalVertexIdx = uniqueVertexIndices.Load((meshlet.vertexOffset + gtid) * 4);
        VertexData v = vertexBuffer[globalVertexIdx];

        // Transform (identity instance for now; will add instance world matrix for instanced draws)
        float4 worldPos = float4(v.Position, 1.0);
        float4 clipPos  = mul(worldPos, viewProj);

        MSOutput o;
        o.svpos    = clipPos;
        o.worldPos = v.Position;
        o.normal   = v.Normal;
        o.uv       = v.UV;
        o.tangent  = v.Tangent;
        o.color    = v.Color;
        o.materialIndex = 0; // TODO: pass from instance data

        verts[gtid] = o;
    }

    // --- Output primitives ---
    if (gtid < meshlet.primitiveCount)
    {
        uint packed = primitiveIndices.Load((meshlet.primitiveOffset + gtid) * 4);
        uint i0 = (packed >>  0) & 0x3FF;
        uint i1 = (packed >> 10) & 0x3FF;
        uint i2 = (packed >> 20) & 0x3FF;
        tris[gtid] = uint3(i0, i1, i2);
    }
}
