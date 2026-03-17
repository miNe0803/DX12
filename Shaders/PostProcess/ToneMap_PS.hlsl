Texture2D<float4> HDRTexture : register(t0);
SamplerState PointSampler : register(s0);

cbuffer Settings : register(b0)
{
    float exposure;
    float gamma;
    float2 padding;
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

float4 main(PSInput input) : SV_TARGET
{
    float3 hdrColor = HDRTexture.SampleLevel(PointSampler, input.uv, 0).rgb;
    hdrColor *= exposure;
    float3 ldrColor = ACESFilm(hdrColor);
    ldrColor = pow(ldrColor, 1.0 / gamma);
    return float4(ldrColor, 1.0);
}
