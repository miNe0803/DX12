// PBR レイヤー（LDR）の上に NPR レイヤー（LDR + A）を合成。透明 NPR は premul 想定で srcOver。
Texture2D<float4> PbrLdrTexture : register(t0);
Texture2D<float4> NprLdrTexture : register(t1);
SamplerState LinearSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 pbr = PbrLdrTexture.SampleLevel(LinearSampler, input.uv, 0).rgb;
    float4 npr = NprLdrTexture.SampleLevel(LinearSampler, input.uv, 0);
    float a = saturate(npr.a);
    // Premultiplied src-over: out = dst * (1 - a) + src.rgb
    float3 outRgb = pbr * (1.0 - a) + npr.rgb;
    return float4(saturate(outRgb), 1.0);
}
