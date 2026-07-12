Texture2D<float4> HDRTexture : register(t0);
Texture2D<float4> BloomTexture : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer Settings : register(b0)
{
    float exposure;
    float gamma;
    float bloomIntensity;
    float gradeSaturation;
    float gradeContrast;
    float gainR;
    float gainG;
    float gainB;             // ホワイトバランス/ゲイン (RGB, 個別floatでパッキング曖昧回避)
};

// カラーグレーディング（線形HDR、ACES カーブ前）。ゲイン→0.18ピボットの power
// コントラスト→彩度。全て中立値(1)で恒等。
float3 ColorGrade(float3 c)
{
    c *= float3(gainR, gainG, gainB);
    c = pow(max(c, 1e-5f), gradeContrast) * pow(0.18f, 1.0f - gradeContrast); // ピボット0.18
    float luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    c = max(lerp(luma.xxx, c, gradeSaturation), 0.0f);
    return c;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Stephen Hill (self-shadow) ACESFitted — RRT+ODT を 3x3 入出力行列で近似。
// Narkowicz 版より UE の filmic ACES に近く、ハイライトのロールオフと彩度保持が良い。
static const float3x3 ACESInputMat =
{
    { 0.59719f, 0.35458f, 0.04823f },
    { 0.07600f, 0.90834f, 0.01566f },
    { 0.02840f, 0.13383f, 0.83777f }
};
static const float3x3 ACESOutputMat =
{
    {  1.60475f, -0.53108f, -0.07367f },
    { -0.10208f,  1.10813f, -0.00605f },
    { -0.00327f, -0.07276f,  1.07602f }
};
float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}
float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
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
    hdrColor = ColorGrade(hdrColor);
    float3 ldrColor = ACESFitted(hdrColor);
    ldrColor = pow(ldrColor, 1.0 / gamma);
    return float4(ldrColor, 1.0);
}
