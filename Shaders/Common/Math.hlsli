#ifndef MATH_HLSLI
#define MATH_HLSLI

float3 ReconstructWorldPos(float2 uv, float depth, matrix invViewProj)
{
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, invViewProj);
    return worldPos.xyz / worldPos.w;
}

#endif
