// ============================================================
// Snow Particle Update Compute Shader
// Updates position, velocity, lifetime of GPU snow particles.
// Respawns dead/out-of-range particles near the camera.
// ============================================================

struct SnowParticle
{
    float3 position;
    float  lifetime;
    float3 velocity;
    float  size;
};

cbuffer SnowCB : register(b0)
{
    float4 CameraPos;     // .xyz = camera world position
    float  DeltaTime;
    float  Time;
    float  SpawnRadius;   // XZ spawn radius around camera (meters)
    float  SpawnHeight;   // Y spawn range above camera (meters)
    float  MaxLifetime;
    float  MinSize;
    float  MaxSize;
    uint   ParticleCount;
    float4 WindParams;    // x=windStrX, y=windStrZ, z=turbulenceScale, w=turbulenceStr
    float  GravityY;      // negative (e.g. -1.5)
    float3 _pad;
};

RWStructuredBuffer<SnowParticle> g_Particles : register(u0);

// Simple hash for pseudo-random
uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

float HashFloat01(uint seed)
{
    return float(WangHash(seed) & 0xFFFFFFu) / float(0xFFFFFFu);
}

float HashRange(uint seed, float minVal, float maxVal)
{
    return lerp(minVal, maxVal, HashFloat01(seed));
}

float3 RandomInCylinder(uint seed, float radius, float height, float3 cameraPos)
{
    float angle = HashFloat01(seed) * 6.28318;
    float r = sqrt(HashFloat01(seed + 1u)) * radius;
    float y = HashFloat01(seed + 2u) * height;
    return float3(
        cameraPos.x + cos(angle) * r,
        cameraPos.y + y,
        cameraPos.z + sin(angle) * r);
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= ParticleCount) return;

    SnowParticle p = g_Particles[tid.x];

    // Wind + turbulence
    float3 wind = float3(
        sin(Time * 0.3) * WindParams.x,
        0,
        cos(Time * 0.5) * WindParams.y);
    float3 turbulence = float3(
        sin(p.position.x * WindParams.z + Time) * WindParams.w,
        0,
        cos(p.position.z * WindParams.z + Time * 0.7) * WindParams.w);

    // Update velocity and position
    p.velocity = float3(0, GravityY, 0) + wind + turbulence;
    p.position += p.velocity * DeltaTime;
    p.lifetime -= DeltaTime;

    // Check if particle needs respawn
    float3 toCamera = p.position - CameraPos.xyz;
    float distSqXZ = dot(toCamera.xz, toCamera.xz);
    bool needsRespawn = (p.lifetime <= 0)
                     || (distSqXZ > SpawnRadius * SpawnRadius)
                     || (p.position.y < CameraPos.y - 20.0); // fell too far below

    if (needsRespawn)
    {
        uint seed = tid.x * 1103515245u + 12345u + asuint(Time * 1000.0);
        p.position = RandomInCylinder(seed, SpawnRadius, SpawnHeight, CameraPos.xyz);
        p.lifetime = HashRange(seed + 7u, 3.0, MaxLifetime);
        p.size     = HashRange(seed + 13u, MinSize, MaxSize);
        p.velocity = float3(0, GravityY, 0);
    }

    g_Particles[tid.x] = p;
}
