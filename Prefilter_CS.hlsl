// Prefiltered Environment Map (GGX importance sampling)
// 1面・1Mip ごとに Dispatch。roughness に応じて鏡面ローブで環境をぼかす。

#define PI 3.14159265359

cbuffer Params : register(b0)
{
    uint faceIndex;
    uint width;
    uint mipLevel;
    float roughness;
}

TextureCube<float4> EnvMap : register(t0);
RWTexture2D<float4> PrefilterFace : register(u0);

SamplerState LinearSampler : register(s0);

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a2 - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float D_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0001);
}

float3 FaceToDirection(uint face, float2 uv)
{
    float u = uv.x * 2.0 - 1.0;
    float v = uv.y * 2.0 - 1.0;
    switch (face)
    {
        case 0: return normalize(float3(1.0, v, u));
        case 1: return normalize(float3(-1.0, -v, u));
        case 2: return normalize(float3(u, 1.0, -v));
        case 3: return normalize(float3(u, -1.0, -v));
        case 4: return normalize(float3(u, -v, 1.0));
        case 5: return normalize(float3(-u, -v, -1.0));
        default: return float3(0, 0, 0);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= width)
        return;

    float2 uv = (float2(DTid.xy) + 0.5) / float(width);
    float3 N = FaceToDirection(faceIndex, uv);
    float3 V = N;

    float3 color = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;
    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; i++)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0)
            continue;

        float NdotH = max(dot(N, H), 0.0001);
        float D = D_GGX(NdotH, roughness);
        float pdf = D * NdotH / (4.0 * max(dot(V, H), 0.0001)) + 0.0001;
        float resolution = float(width);
        float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
        float mipLevel = 0.5 * log2(saTexel / max(pdf * float(SAMPLE_COUNT), 0.0001));
        mipLevel = clamp(mipLevel, 0.0, 10.0);

        float3 envColor = EnvMap.SampleLevel(LinearSampler, L, mipLevel).rgb;
        color += envColor * NdotL;
        totalWeight += NdotL;
    }
    color /= max(totalWeight, 0.0001);
    PrefilterFace[DTid.xy] = float4(color, 1.0);
}
