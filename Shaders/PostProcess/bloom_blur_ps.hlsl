Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer BlurParams : register(b0)
{
    float2 texelSize;
    float2 direction;
    float blurSize;
    float3 padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// 5-tap separable Gaussian weights (approx)
static const float weight[3] = { 0.227027, 0.316216, 0.070270 }; // center, offset1, offset2 (x2)
static const float offset[2] = { 1.0, 2.0 };

float4 main(PSInput input) : SV_TARGET
{
    float2 off = direction * texelSize * blurSize;
    float4 acc = SourceTexture.SampleLevel(LinearSampler, input.uv, 0) * weight[0];
    acc += SourceTexture.SampleLevel(LinearSampler, input.uv + off * offset[0], 0) * weight[1];
    acc += SourceTexture.SampleLevel(LinearSampler, input.uv - off * offset[0], 0) * weight[1];
    acc += SourceTexture.SampleLevel(LinearSampler, input.uv + off * offset[1], 0) * weight[2];
    acc += SourceTexture.SampleLevel(LinearSampler, input.uv - off * offset[1], 0) * weight[2];
    return acc;
}
