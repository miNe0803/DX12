// GTAOApply_PS.hlsl — AO を HDR に乗算適用（PSO は乗算ブレンド Src=DEST_COLOR/Dest=ZERO）。
// 3x3 ボックスブラーで SSAO のノイズを均す。フルスクリーン三角(ToneMap_VS)で駆動。
cbuffer AoCb : register(b0)
{
    matrix InvViewProj;
    float4 CamPos;
    float2 InvRes;
    float  Radius;
    float  Strength;
    float  Bias;
    float  MaxDist;
    float2 _pad;
};
Texture2D<float> AO : register(t0);
SamplerState     Smp : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput i) : SV_TARGET
{
    float a = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        a += AO.SampleLevel(Smp, i.uv + float2(x, y) * InvRes, 0);
    a *= (1.0f / 9.0f);
    return float4(a, a, a, 1.0f);   // 乗算ブレンドで HDR *= a
}
