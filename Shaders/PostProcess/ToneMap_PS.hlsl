Texture2D<float4> HDRTexture : register(t0);
Texture2D<float4> BloomTexture : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer Settings : register(b0)
{
    float exposure;
    float gamma;
    float bloomIntensity;
    float padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float3 ACESFilm(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Inf/NaN を 0 にし、上限でクランプ（太陽等の極端な明るさで黒点になるのを防ぐ）
float3 SanitizeHDR(float3 c, float maxVal)
{
    float3 r = c;
    r = select((isnan(r) | isinf(r)), float3(0, 0, 0), r);
    return min(r, maxVal);
}

float4 main(PSInput input) : SV_TARGET
{
    float3 hdrColor = HDRTexture.SampleLevel(LinearSampler, input.uv, 0).rgb;
    float3 bloomColor = BloomTexture.SampleLevel(LinearSampler, input.uv, 0).rgb;
    hdrColor = SanitizeHDR(hdrColor, 1e4);
    bloomColor = SanitizeHDR(bloomColor, 1e4);
    hdrColor += bloomColor * bloomIntensity;
    hdrColor *= exposure;
    float3 ldrColor = ACESFilm(hdrColor);
    ldrColor = pow(ldrColor, 1.0 / gamma);
    return float4(ldrColor, 1.0);
}
