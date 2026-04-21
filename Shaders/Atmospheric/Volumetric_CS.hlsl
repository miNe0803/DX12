#include "../Common/Math.hlsli"
#include "../Common/Lighting.hlsli"

cbuffer AtmosphereCB : register(b0)
{
    matrix InvViewProj;
    float4 CameraPos;
    float4 SunDirection;   // .xyz = normalised dir TO light
    float4 SunColor;       // .rgb
    float4 FogParams;      // x=density, y=scatteringG, z=heightFalloff, w=baseHeight
    float4 FrameParams;    // x=frameIndex, y=quarterW, z=quarterH, w=noiseStrength
};

cbuffer ShadowCB : register(b1)
{
    matrix LightVP[4];
    float4 CascadeSplits; // .x/.y/.z/.w = cascade 0/1/2/3
};

Texture2D<float>       SceneDepth : register(t0);
Texture2DArray<float>  ShadowMap  : register(t1);
SamplerComparisonState shadowSmp  : register(s0);
SamplerState           pointSmp   : register(s1);
RWTexture2D<float4>    OutVolume  : register(u0);

static const int   NUM_STEPS = 8; // フォグ品質は temporal reprojection で補完
static const float kShadowBias = 0.002;

// ---- Procedural 3D noise (no texture needed) ----

float hash3to1(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f); // smoothstep

    float n000 = hash3to1(i + float3(0, 0, 0));
    float n100 = hash3to1(i + float3(1, 0, 0));
    float n010 = hash3to1(i + float3(0, 1, 0));
    float n110 = hash3to1(i + float3(1, 1, 0));
    float n001 = hash3to1(i + float3(0, 0, 1));
    float n101 = hash3to1(i + float3(1, 0, 1));
    float n011 = hash3to1(i + float3(0, 1, 1));
    float n111 = hash3to1(i + float3(1, 1, 1));

    float x0 = lerp(n000, n100, f.x);
    float x1 = lerp(n010, n110, f.x);
    float x2 = lerp(n001, n101, f.x);
    float x3 = lerp(n011, n111, f.x);

    float y0 = lerp(x0, x1, f.y);
    float y1 = lerp(x2, x3, f.y);

    return lerp(y0, y1, f.z);
}

// FBM: 3 octaves for natural-looking density variation
float FBM3D(float3 p)
{
    float v = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        v += amp * ValueNoise3D(p * freq);
        freq *= 2.0;
        amp *= 0.5;
    }
    return v;
}

// ---- Shadow sampling ----

float SampleShadowCascade(float3 worldPos, uint cascade)
{
    float4 lc = mul(float4(worldPos, 1.0), LightVP[cascade]);
    float3 pc = lc.xyz / lc.w;
    float2 uv = pc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return -1.0;
    return ShadowMap.SampleCmpLevelZero(shadowSmp, float3(uv, (float)cascade), pc.z - kShadowBias);
}

float SampleShadow(float3 worldPos)
{
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        float s = SampleShadowCascade(worldPos, c);
        if (s >= 0.0) return s;
    }
    return 1.0;
}

// ---- Main ----

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float quarterW = FrameParams.y;
    float quarterH = FrameParams.z;
    if (DTid.x >= (uint)quarterW || DTid.y >= (uint)quarterH)
        return;

    float2 uv = (float2(DTid.xy) + 0.5) / float2(quarterW, quarterH);
    float depth = SceneDepth.SampleLevel(pointSmp, uv, 0);

    if (depth >= 1.0)
    {
        OutVolume[DTid.xy] = float4(0, 0, 0, 1);
        return;
    }

    float3 worldPos = ReconstructWorldPos(uv, depth, InvViewProj);

    float3 rayStart = CameraPos.xyz;
    float3 rayEnd   = worldPos;
    float  rayLength = length(rayEnd - rayStart);
    float3 rayDir    = (rayEnd - rayStart) / max(rayLength, 1e-5);

    uint frameHash = (uint)FrameParams.x & 0xF;
    float jitter = frac(0.5 + float(frameHash) * 0.618033988);

    float density     = FogParams.x;
    float scatteringG = FogParams.y;
    float heightFall  = FogParams.z;
    float baseHeight  = FogParams.w;
    float noiseStrength = SunColor.a;
    float stepSize    = rayLength / float(NUM_STEPS);

    // Slow wind animation: shifts noise coordinates over time
    float time = FrameParams.x * 0.02;
    float3 windOffset = float3(time * 0.3, 0, time * 0.15);

    float3 totalScattering = 0;
    float  totalTransmittance = 1.0;

    for (int i = 0; i < NUM_STEPS; ++i)
    {
        float t = (float(i) + jitter) / float(NUM_STEPS);
        float3 samplePos = lerp(rayStart, rayEnd, t);

        float heightFactor = exp(-heightFall * max(samplePos.y - baseHeight, 0));

        // 3D noise density modulation: pockets of thicker/thinner fog
        float noiseDensity = 1.0;
        if (noiseStrength > 0.001)
        {
            float noiseVal = FBM3D(samplePos * 0.02 + windOffset);
            noiseDensity = lerp(1.0, noiseVal * 2.0, noiseStrength);
            noiseDensity = max(noiseDensity, 0.0);
        }

        float localDensity = density * heightFactor * noiseDensity;

        float shadow = SampleShadow(samplePos);
        float cosTheta = dot(rayDir, SunDirection.xyz);
        float phase = HenyeyGreenstein(cosTheta, scatteringG);

        float3 inscatter = SunColor.rgb * phase * shadow * localDensity * stepSize;
        float  extinction = localDensity * stepSize;

        totalScattering += inscatter * totalTransmittance;
        totalTransmittance *= exp(-extinction);
    }

    OutVolume[DTid.xy] = float4(totalScattering, totalTransmittance);
}
