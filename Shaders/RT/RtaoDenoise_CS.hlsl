// ============================================================
//  RtaoDenoise_CS.hlsl — RTAO 時間デノイズ（静的再投影, SM6.6）。
//  現フレームの生AO を、前フレームAO を PrevViewProj で再投影して混合し、少数レイの
//  ノイズを時間方向に平均化する。町は静的なのでカメラ移動のみの再投影は幾何的に厳密。
//  AtmosphereSystem の Temporal_CS を AO(1ch) 用に移植（現フレーム深度のみ・prev深度不要）。
//  近傍クランプでゴースト（遮蔽物シルエットの尾引き）を抑制。
// ============================================================
#include "../Common/Math.hlsli"

cbuffer DenoiseCb : register(b0)
{
    matrix InvViewProj;    // 現フレーム
    matrix PrevViewProj;   // 前フレーム（row-major, 転置済）
    float2 InvRes;         // (1/aoW, 1/aoH) 半解像度
    float  BlendAlpha;     // 現フレーム混合率（小さいほど滑らか）
    uint   HasHistory;     // 0=履歴無効（1フレーム目）→生AOをそのまま
};

Texture2D<float>   Depth   : register(t0);
Texture2D<float>   RawAO   : register(t1);
Texture2D<float>   PrevAO  : register(t2);
SamplerState       linearSmp : register(s0);
SamplerState       pointSmp  : register(s1);
RWTexture2D<float> OutAO   : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint W, H; RawAO.GetDimensions(W, H);
    if (id.x >= W || id.y >= H) return;

    float2 uv = (float2(id.xy) + 0.5f) * InvRes;
    float raw = RawAO[id.xy];
    float d = Depth.SampleLevel(pointSmp, uv, 0);

    // 空 or 履歴無し → 生AOをそのまま
    if (d >= 1.0f || HasHistory == 0u) { OutAO[id.xy] = raw; return; }

    // ワールド復元 → 前フレームクリップへ再投影
    float3 wp = ReconstructWorldPos(uv, d, InvViewProj);
    float4 pc = mul(float4(wp, 1.0f), PrevViewProj);
    float2 pUV = (pc.xy / pc.w) * 0.5f + 0.5f; pUV.y = 1.0f - pUV.y;

    // ディスオクルージョン: 画面外は棄却（生AOにフォールバック）
    bool valid = (pc.w > 0.0f && pUV.x >= 0.0f && pUV.x <= 1.0f && pUV.y >= 0.0f && pUV.y <= 1.0f);
    if (!valid) { OutAO[id.xy] = raw; return; }

    float hist = PrevAO.SampleLevel(linearSmp, pUV, 0);

    // 近傍クランプ: 履歴を現フレーム 3x3 の [min,max] に収める（尾引き抑制）
    float mn = raw, mx = raw;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 c = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2((int)W - 1, (int)H - 1));
        float s = RawAO[c];
        mn = min(mn, s); mx = max(mx, s);
    }
    hist = clamp(hist, mn, mx);

    OutAO[id.xy] = lerp(hist, raw, BlendAlpha);
}
