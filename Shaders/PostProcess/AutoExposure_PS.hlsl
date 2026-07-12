// ============================================================
//  AutoExposure_PS.hlsl — HDR シーンの平均輝度（幾何平均=log2平均）を
//  1x1 の R32_FLOAT ターゲットへ縮約する。1 ピクセルで NxN グリッドを
//  サンプルし、log2(luma) の平均を出力。CPU が読み戻して露出を決める。
//  VS は ToneMap_VS を流用（フルスクリーン三角形）。
// ============================================================
Texture2D<float4> g_HDR : register(t0);
SamplerState      g_Smp : register(s0);

cbuffer AeCb : register(b0)
{
    uint  SampleDim;   // グリッド 1 辺（例 64 → 4096 サンプル）
    float MinLogLum;   // 下限クランプ（log2 luminance）
    float MaxLogLum;   // 上限クランプ
    float _pad;
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float main(PSIn i) : SV_TARGET
{
    uint n = max(SampleDim, 1u);
    float logSum = 0.0f;
    [loop] for (uint y = 0; y < n; ++y)
    {
        [loop] for (uint x = 0; x < n; ++x)
        {
            float2 uv = (float2(x, y) + 0.5f) / (float)n;
            float3 c = g_HDR.SampleLevel(g_Smp, uv, 0).rgb;
            float lum = max(dot(c, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
            logSum += clamp(log2(lum), MinLogLum, MaxLogLum);
        }
    }
    return logSum / (float)(n * n);   // 平均 log2 輝度（幾何平均の指数）
}
