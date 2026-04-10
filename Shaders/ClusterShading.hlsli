#ifndef CLUSTER_SHADING_HLSLI
#define CLUSTER_SHADING_HLSLI

// ============================================================
// Forward+ Clustered Shading — shared types and utilities
// ============================================================

#define CLUSTER_TILE_SIZE     16
#define CLUSTER_DEPTH_SLICES  24
#define MAX_LIGHTS_PER_CLUSTER 128

struct LightData
{
    float3 position;
    float  range;
    float3 color;
    float  intensity;
    float3 direction;    // spot only
    float  spotAngleCos; // cos(outerCone), 0=point
    uint   type;         // 0=point, 1=spot
    float  spotInnerCos;
    float2 _pad;
};

struct ClusterEntry
{
    uint offset;
    uint count;
};

/// Compute the cluster index for a pixel given screen position and linear view depth.
uint3 ComputeClusterId(float2 screenPos, float linearDepth,
                        float logScale, float logBias,
                        uint tileCountX)
{
    uint tileX = (uint)screenPos.x / CLUSTER_TILE_SIZE;
    uint tileY = (uint)screenPos.y / CLUSTER_TILE_SIZE;
    uint slice = (uint)max(0.0, log2(linearDepth) * logScale + logBias);
    slice = min(slice, CLUSTER_DEPTH_SLICES - 1);
    return uint3(tileX, tileY, slice);
}

uint FlattenClusterId(uint3 id, uint tileCountX, uint tileCountY)
{
    return id.z * (tileCountX * tileCountY) + id.y * tileCountX + id.x;
}

/// Evaluate a point light contribution (Frostbite-style attenuation).
float3 EvaluatePointLight(LightData light, float3 worldPos, float3 N, float3 V,
                           float3 albedo, float metallic, float roughness)
{
    float3 L = light.position - worldPos;
    float dist = length(L);
    if (dist > light.range) return float3(0, 0, 0);
    L /= dist;

    // Smooth attenuation (window function)
    float attenuation = saturate(1.0 - pow(dist / light.range, 4.0));
    attenuation *= attenuation;
    attenuation /= (dist * dist + 1.0);

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return float3(0, 0, 0);

    // Simplified BRDF (diffuse Lambert + Blinn-Phong specular)
    float3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float specPower = max(2.0 / (roughness * roughness + 0.001) - 2.0, 1.0);
    float spec = pow(NdotH, specPower) * (specPower + 2.0) / 8.0;

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse = albedo * (1.0 - metallic) / 3.14159;
    float3 specular = F0 * spec;

    // Spot attenuation
    if (light.type == 1 && light.spotAngleCos > 0.0)
    {
        float cosAngle = dot(-L, light.direction);
        float spotFade = saturate((cosAngle - light.spotAngleCos) /
                                  (light.spotInnerCos - light.spotAngleCos));
        attenuation *= spotFade;
    }

    return (diffuse + specular) * light.color * light.intensity * NdotL * attenuation;
}

#endif // CLUSTER_SHADING_HLSLI
