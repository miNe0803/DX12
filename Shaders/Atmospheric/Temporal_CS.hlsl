// Temporal reprojection for volumetric light:
// Blends current quarter-res volumetric with the reprojected previous frame
// to remove single-frame jittering noise from ray marching.

#include "../Common/Math.hlsli"

cbuffer TemporalCB : register(b0)
{
    matrix InvViewProj;      // current frame
    matrix PrevViewProj;     // previous frame (row-major, transposed for HLSL)
    float4 CameraPos;
    float4 TemporalParams;   // x=blendAlpha (e.g. 0.05), y=quarterW, z=quarterH, w=frameIndex
};

Texture2D<float>  SceneDepth   : register(t0);
Texture2D<float4> CurrentVol   : register(t1);
Texture2D<float4> PrevVol      : register(t2);
SamplerState      linearSmp    : register(s0);
SamplerState      pointSmp     : register(s1);
RWTexture2D<float4> OutVol     : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float quarterW = TemporalParams.y;
    float quarterH = TemporalParams.z;
    if (DTid.x >= (uint)quarterW || DTid.y >= (uint)quarterH)
        return;

    float2 uv = (float2(DTid.xy) + 0.5) / float2(quarterW, quarterH);

    float4 current = CurrentVol[DTid.xy];
    float depth = SceneDepth.SampleLevel(pointSmp, uv, 0);

    if (depth >= 1.0)
    {
        OutVol[DTid.xy] = float4(0, 0, 0, 1);
        return;
    }

    // Reconstruct world position and reproject to previous frame UV
    float3 worldPos = ReconstructWorldPos(uv, depth, InvViewProj);
    float4 prevClip = mul(float4(worldPos, 1.0), PrevViewProj);
    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUV = prevNdc * 0.5 + 0.5;
    prevUV.y = 1.0 - prevUV.y;

    float blendAlpha = TemporalParams.x;

    // Disocclusion: reject if reprojected UV is out of screen
    bool valid = (prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                  prevUV.y >= 0.0 && prevUV.y <= 1.0 &&
                  prevClip.w > 0.0);

    if (!valid)
    {
        OutVol[DTid.xy] = current;
        return;
    }

    float4 history = PrevVol.SampleLevel(linearSmp, prevUV, 0);

    // Neighbourhood clamping: clamp history to [min,max] of 3x3 current samples
    // to prevent ghosting from stale data
    float4 nMin = current, nMax = current;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 coord = int2(DTid.xy) + int2(x, y);
        coord = clamp(coord, int2(0, 0), int2((int)quarterW - 1, (int)quarterH - 1));
        float4 s = CurrentVol[coord];
        nMin = min(nMin, s);
        nMax = max(nMax, s);
    }
    history = clamp(history, nMin, nMax);

    // Blend: mostly keep history (smooth), small portion of current (responsive)
    OutVol[DTid.xy] = lerp(history, current, blendAlpha);
}
