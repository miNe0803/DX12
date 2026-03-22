Texture2D<float4> HDRTexture : register(t0);
SamplerState PointSampler : register(s0);

cbuffer BloomParams : register(b0)
{
    float threshold;
    float kneeWidth;
    float2 padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

static const float kMaxBloomExtract = 65504.0;

// 旧実装: 輝度が閾値を超えると「フルカラー」を Bloom に流していたため、肌が閾値付近で丸ごと発光していた。
// 現在: 閾値を超えた分（エッジ）だけを色方向に乗算して抽出。kneeWidth>0 でニーを付けて閾値直近をさらに抑える。
float4 main(PSInput input) : SV_TARGET
{
    float3 color = HDRTexture.SampleLevel(PointSampler, input.uv, 0).rgb;
    color = select((isnan(color) | isinf(color)), float3(0, 0, 0), color);
    color = min(color, kMaxBloomExtract);
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float excess = max(0.0, luminance - threshold);
    float response = excess;
    if (kneeWidth > 1e-6)
        response = excess * excess / (excess + kneeWidth);
    float3 bloom = color * (response / max(luminance, 1e-4));
    return float4(bloom, 1.0);
}
