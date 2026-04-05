// Fog + volumetric composite via hardware alpha blending.
// Blend mode: Final = Output.rgb * ONE + Scene.rgb * SRC_ALPHA
// Output: RGB = fog inscatter + volumetric inscatter
//         A   = transmittance (scene colour multiplier)
// Bilateral upsample: reads 4 nearest quarter-res texels weighted by depth similarity.

cbuffer FogCB : register(b0)
{
    matrix InvViewProj;
    float4 CameraPos;
    float4 SunDirection;
    float4 SunColor;
    float4 FogParams;    // x=density, y=scatteringG, z=heightFalloff, w=baseHeight
    float4 FrameParams;  // x=frameIndex, y=quarterW, z=quarterH, w=volumetricActive
    float4 FogColor;     // .rgb = base fog colour, .a = maxFogDistance
};

SamplerState pointSmp  : register(s0);
SamplerState linearSmp : register(s1);
Texture2D<float>  SceneDepth    : register(t0);
Texture2D<float4> VolumetricTex : register(t1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clip = float4(ndc, depth, 1.0);
    float4 wp = mul(clip, InvViewProj);
    return wp.xyz / wp.w;
}

// Bilateral upsample: sample 4 nearest quarter-res texels,
// weight by depth similarity to avoid bleeding across depth edges.
float4 BilateralUpsampleVol(float2 fullResUV, float fullResDepth)
{
    float2 quarterSize = float2(FrameParams.y, FrameParams.z);
    float2 quarterTexelSize = 1.0 / quarterSize;

    // Map full-res UV to quarter-res texel coordinate
    float2 quarterCoord = fullResUV * quarterSize - 0.5;
    float2 baseTexel = floor(quarterCoord);
    float2 frac_ = quarterCoord - baseTexel;

    // Depth sensitivity: lower = more blurry, higher = sharper edges
    static const float kDepthSigma = 0.01;

    float4 result = 0;
    float totalWeight = 0;

    [unroll] for (int y = 0; y <= 1; ++y)
    [unroll] for (int x = 0; x <= 1; ++x)
    {
        int2 texel = int2(baseTexel) + int2(x, y);
        texel = clamp(texel, int2(0, 0), int2(quarterSize) - int2(1, 1));

        float2 sampleUV = (float2(texel) + 0.5) * quarterTexelSize;
        float sampleDepth = SceneDepth.SampleLevel(pointSmp, sampleUV, 0);

        float depthDiff = abs(fullResDepth - sampleDepth);
        float depthWeight = exp(-depthDiff * depthDiff / (kDepthSigma * kDepthSigma));

        // Bilinear weight
        float bx = (x == 0) ? (1.0 - frac_.x) : frac_.x;
        float by = (y == 0) ? (1.0 - frac_.y) : frac_.y;
        float bilinearWeight = bx * by;

        float w = bilinearWeight * depthWeight;
        result += VolumetricTex[texel] * w;
        totalWeight += w;
    }

    return (totalWeight > 1e-6) ? (result / totalWeight) : VolumetricTex.SampleLevel(linearSmp, fullResUV, 0);
}

float4 main(PSInput input) : SV_TARGET
{
    float depth = SceneDepth.SampleLevel(pointSmp, input.uv, 0);

    // Skip skybox: depth buffer cleared to 1.0, skybox doesn't write depth.
    if (depth >= 1.0)
        return float4(0, 0, 0, 1);

    float3 worldPos = ReconstructWorldPos(input.uv, depth);

    // Exponential height fog with distance clamping
    float maxDist = FogColor.a;
    float dist = min(length(worldPos - CameraPos.xyz), maxDist);
    float heightFactor = exp(-FogParams.z * max(worldPos.y - FogParams.w, 0));
    float fogAmount = 1.0 - exp(-FogParams.x * dist * heightFactor);
    fogAmount = saturate(fogAmount);

    // Inscattering colour: sun-facing gets warm tint
    float3 viewDir = normalize(worldPos - CameraPos.xyz);
    float sunDot = max(dot(viewDir, SunDirection.xyz), 0.0);
    float3 inscatterColor = lerp(FogColor.rgb, SunColor.rgb * 0.5, pow(sunDot, 8.0));
    float3 fogInscatter = inscatterColor * fogAmount;

    // Volumetric light with bilateral upsampling
    float4 vol = float4(0, 0, 0, 1);
    if (FrameParams.w > 0.5)
        vol = BilateralUpsampleVol(input.uv, depth);

    // Single-medium extinction: fog transmittance handles attenuation,
    // volumetric inscatter (directional god rays) adds on top.
    float fogTransmittance = 1.0 - fogAmount;
    float3 totalInscatter = fogInscatter + vol.rgb;

    return float4(totalInscatter, fogTransmittance);
}
