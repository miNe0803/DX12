#include "../Common/Math.hlsli"
#include "../Common/Lighting.hlsli"

cbuffer AtmosphereCB : register(b0)
{
    matrix InvViewProj;
    float4 CameraPos;
    float4 SunDirection;   // .xyz = normalised dir TO light
    float4 SunColor;       // .rgb
    float4 FogParams;      // x=density, y=scatteringG, z=heightFalloff, w=baseHeight
    float4 FrameParams;    // x=frameIndex, y=quarterW, z=quarterH, w=unused
};

cbuffer ShadowCB : register(b1)
{
    matrix LightVP[3];
    float4 CascadeSplits;
};

Texture2D<float>       SceneDepth : register(t0);
Texture2DArray<float>  ShadowMap  : register(t1);
SamplerComparisonState shadowSmp  : register(s0);
SamplerState           pointSmp   : register(s1);
RWTexture2D<float4>    OutVolume  : register(u0);

static const int   NUM_STEPS = 24;
static const float kShadowBias = 0.002;

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

    // Blue-noise-style jitter from frame index
    uint frameHash = (uint)FrameParams.x & 0xF;
    float jitter = frac(0.5 + float(frameHash) * 0.618033988);

    float density     = FogParams.x;
    float scatteringG = FogParams.y;
    float heightFall  = FogParams.z;
    float baseHeight  = FogParams.w;
    float stepSize    = rayLength / float(NUM_STEPS);

    float3 totalScattering = 0;
    float  totalTransmittance = 1.0;

    for (int i = 0; i < NUM_STEPS; ++i)
    {
        float t = (float(i) + jitter) / float(NUM_STEPS);
        float3 samplePos = lerp(rayStart, rayEnd, t);

        float heightFactor = exp(-heightFall * max(samplePos.y - baseHeight, 0));
        float localDensity = density * heightFactor;

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
