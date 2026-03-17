Texture2D<float4> HDRTexture : register(t0);
SamplerState PointSampler : register(s0);

cbuffer BloomParams : register(b0)
{
    float threshold;
    float3 padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Inf/NaN を 0 にし、上限でクランプ（Bloom 経路に極端な値が流れて黒点になるのを防ぐ）
static const float kMaxBloomExtract = 65504.0; // half float 最大付近

float4 main(PSInput input) : SV_TARGET
{
    float3 color = HDRTexture.SampleLevel(PointSampler, input.uv, 0).rgb;
    color = select((isnan(color) | isinf(color)), float3(0, 0, 0), color);
    color = min(color, kMaxBloomExtract);
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= threshold)
        return float4(0.0, 0.0, 0.0, 1.0);
    return float4(color, 1.0);
}
