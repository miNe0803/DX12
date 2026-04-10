#ifndef BINDLESS_HLSLI
#define BINDLESS_HLSLI

// ============================================================
// SM6.6 Bindless Resource Access
// Requires root signature flag: CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
// ============================================================

// --- Shading model IDs ---
#define SHADING_MODEL_PBR   0
#define SHADING_MODEL_NPR   1
#define SHADING_MODEL_WATER 2

// --- Material data (must match C++ GpuMaterialData, 80 bytes) ---
struct MaterialData
{
    uint albedoTexIdx;
    uint normalTexIdx;
    uint metallicTexIdx;
    uint roughnessTexIdx;
    uint rampTexIdx;          // NPR ramp (0xFFFFFFFF = unused)
    uint sphereMapTexIdx;     // NPR PMX sphere
    uint shadowMapArrayIdx;
    uint prefilterEnvIdx;     // IBL
    uint irradianceMapIdx;    // IBL
    uint brdfLutIdx;          // IBL
    uint shadingModel;        // 0=PBR, 1=NPR, 2=Water
    float celSharpness;       // NPR
    float rimPower;           // NPR
    float rimStrength;        // NPR
    float nprExposureOverride;// NPR tone
    uint sphereMode;          // PMX 0/1/2
    float celVertexBlend;     // NPR
    float nprOpacity;         // NPR
    float2 _pad;
};

// --- Root constants (b2, space0) — set per draw ---
cbuffer DrawConstants : register(b2, space0)
{
    uint g_MaterialBufferIdx;   // heap index of StructuredBuffer<MaterialData>
    uint g_InstanceBufferIdx;   // heap index of StructuredBuffer<InstanceData>
    uint g_DrawID;              // base instance or draw identifier
    uint g_Reserved;
};

// --- Instance data (must match C++ InstanceData, 80 bytes) ---
struct InstanceData
{
    float4x4 World;             // 64 bytes, row_major
    uint     materialIndex;     // 4 bytes
    uint3    _pad;              // 12 bytes
};

// --- Helper: fetch material buffer from bindless heap ---
MaterialData FetchMaterial(uint materialIndex)
{
    StructuredBuffer<MaterialData> materials = ResourceDescriptorHeap[g_MaterialBufferIdx];
    return materials[materialIndex];
}

// --- Helper: fetch instance data from bindless heap ---
InstanceData FetchInstance(uint instanceID)
{
    StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[g_InstanceBufferIdx];
    return instances[g_DrawID + instanceID];
}

// --- Helper: sample a bindless Texture2D ---
float4 SampleBindlessTex2D(uint heapIdx, SamplerState smp, float2 uv)
{
    Texture2D<float4> tex = ResourceDescriptorHeap[heapIdx];
    return tex.Sample(smp, uv);
}

// --- Helper: sample a bindless TextureCube ---
float4 SampleBindlessTexCube(uint heapIdx, SamplerState smp, float3 dir)
{
    TextureCube<float4> tex = ResourceDescriptorHeap[heapIdx];
    return tex.Sample(smp, dir);
}

// --- Helper: sample a bindless TextureCube at specific LOD ---
float4 SampleBindlessTexCubeLod(uint heapIdx, SamplerState smp, float3 dir, float lod)
{
    TextureCube<float4> tex = ResourceDescriptorHeap[heapIdx];
    return tex.SampleLevel(smp, dir, lod);
}

// --- Helper: sample shadow map (Texture2DArray) with comparison ---
float SampleBindlessShadowCmp(uint heapIdx, SamplerComparisonState cmpSmp, float3 uvSlice, float cmpValue)
{
    Texture2DArray<float> shadowMap = ResourceDescriptorHeap[heapIdx];
    return shadowMap.SampleCmpLevelZero(cmpSmp, uvSlice, cmpValue);
}

// --- Helper: check if a texture index is valid ---
bool IsValidTexIdx(uint idx) { return idx != 0xFFFFFFFF; }

#endif // BINDLESS_HLSLI
