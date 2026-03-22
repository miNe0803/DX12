Texture2D<float4> NprHdrTexture : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer NprPostSettings : register(b0)
{
    float nprExposure;
    float nprGamma;
    float2 padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // 1. テクスチャの絵をそのまま持ってくる
    float4 c = NprHdrTexture.SampleLevel(LinearSampler, input.uv, 0);
    
    // 2. 露出（Exposure）だけ掛ける
    c.rgb *= nprExposure;
    
    // 3. トーンマップもガンマ補正も一切せずに、1.0で天井を叩いて（クランプして）出力！
    return float4(saturate(c.rgb), c.a);
}
