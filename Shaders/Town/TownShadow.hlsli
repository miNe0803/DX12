#ifndef TOWN_SHADOW_HLSLI
#define TOWN_SHADOW_HLSLI
// ============================================================
//  TownShadow.hlsli — 町の共有ソフトシャドウ（UE5 品質に寄せる）。
//  16-tap Vogel ディスク PCF + 画素毎回転(interleaved gradient noise) +
//  法線オフセットバイアス。カスケードは viewDepth で選択。
//
//  インクルード側が事前に宣言していること:
//    Texture2DArray          g_Csm       : register(t0, space2);
//    SamplerComparisonState  g_ShadowSmp : register(s1, space2);
//    cbuffer ShadowCB { matrix LightVP[4]; float4 CascadeSplits; };
//    cbuffer SceneCB  { matrix View; ... };
//
//  解像度は ShadowSystem::kShadowMapSize と一致させる（下記 TS_ShadowSize）。
// ============================================================

static const float TS_ShadowSize = 2048.0f;   // = ShadowSystem::kShadowMapSize
static const float TS_ConstBias  = 0.0015f;    // 比較深度の定数バイアス（ortho なので線形）
static const float TS_Radius     = 2.6f;       // PCF 半径（テクセル）＝ペナンブラの柔らかさ
static const int   TS_Taps       = 16;

// ワールド空間ハッシュ（0..1）。回転シードに使う。**ワールド座標に固定**なので、
// カメラが動いても同一ワールド点は同じ回転＝ペナンブラのちらつき/クロールが出ない。
// （スクリーン空間シードだと画素の下を内容がスクロール→ディザが泳いで「ギザギザ揺れ」る）。
float TS_Hash(float3 p)
{
    return frac(sin(dot(p, float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
}

// Vogel ディスク上のサンプル点（i∈[0,n), 黄金角螺旋, rot で回転）。
float2 TS_Vogel(int i, int n, float rot)
{
    float r = sqrt((i + 0.5f) / n);
    float theta = i * 2.39996323f + rot;   // 黄金角
    float s, c; sincos(theta, s, c);
    return r * float2(c, s);
}

// viewDepth からカスケード選択（0..3, 最遠より先は 99=無影）。
uint TS_SelectCascade(float3 worldPos)
{
    float vz = mul(float4(worldPos, 1.0f), View).z;
    if (vz < CascadeSplits.x) return 0u;
    if (vz < CascadeSplits.y) return 1u;
    if (vz < CascadeSplits.z) return 2u;
    if (vz < CascadeSplits.w) return 3u;
    return 99u;
}

// CascadeTexelWorld[cascade] を動的 vector 添字を避けて取得（world m/テクセル）。
float TS_TexelW(uint c)
{
    return (c == 0u) ? CascadeTexelWorld.x :
           (c == 1u) ? CascadeTexelWorld.y :
           (c == 2u) ? CascadeTexelWorld.z : CascadeTexelWorld.w;
}

// 指定カスケードを固定16-tap Vogelでサンプル。1=光, 0=影。uv 範囲外は 1.0(光)。noff=法線オフセット(world m)。
// 回転無し＝画素毎/フレーム毎に不変でちらつかない（worldハッシュ回転は微小揺れに過敏で逆効果だった）。
float TS_SampleCascade(float3 worldPos, float3 N, uint cascade, float noff)
{
    float3 wp = worldPos + N * noff;
    float4 lc = mul(float4(wp, 1.0f), LightVP[cascade]);
    float3 p  = lc.xyz / lc.w;
    float2 uv = p.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return 1.0f;

    float cmp = p.z - TS_ConstBias;
    // BUG3: PCF 半径をカスケード毎に調整し、**世界空間ペナンブラ幅をカスケード0に統一**する。
    // 粗い(遠い)カスケードほど少ないテクセルを使う → 遷移帯で c と c+1 の影縁が一致し、
    // 二者を lerp してもクロールしなくなる（従来は texel 固定で c+1 が約2倍ボケていた）。
    float invSize = (TS_Radius / TS_ShadowSize) * (CascadeTexelWorld.x / max(TS_TexelW(cascade), 1e-4f));
    float sum = 0.0f;
    [unroll] for (int i = 0; i < TS_Taps; ++i)
    {
        float2 o = TS_Vogel(i, TS_Taps, 0.0f) * invSize;
        sum += g_Csm.SampleCmpLevelZero(g_ShadowSmp, float3(uv + o, (float)cascade), cmp);
    }
    return sum * (1.0f / (float)TS_Taps);
}

// ソフト太陽シャドウ。1=光, 0=影。N=面法線(bias用), pixXY=SV_Position.xy(未使用: API互換)。
// カスケードを viewZ で選択し、split 手前の帯で次カスケードへクロスフェード＝継ぎ目の段差/移動時
// のクロールを解消（隣接カスケードはテクセル密度が違い縁が微妙にズレるため、移動で境界が泳ぐ）。
float SampleSunShadowSoft(float3 worldPos, float3 N, float2 pixXY)
{
    float vz = mul(float4(worldPos, 1.0f), View).z;
    if (vz >= CascadeSplits.w) return 1.0f;   // 最遠カスケードより先＝無影（view-Z far で判定）

    // BUG3: カスケード選択を **カメラからの半径距離 d** で行う（回転不変）。従来は vz(=view-Z)で
    // 選択していたため、同じワールド点でもカメラを回すと vz=D·cosθ が split0 を跨いで
    // カスケードが切り替わり、境界の lerp が泳いで揺れ＋cascade0/1 間で影が出没していた。
    // d は回転で不変（同じ位置なら同じ点は常に同じ距離）→ 回転由来の揺れ/出没が消える。
    // d>=vz なので選ぶカスケードは vz 判定と同等以上に遠い＝必ず箱に収まる（穴は出ない）。
    float d = length(worldPos - CameraWorld.xyz);

    uint  c;
    float splitNear, splitFar;
    if      (d < CascadeSplits.x) { c = 0u; splitNear = 0.0f;            splitFar = CascadeSplits.x; }
    else if (d < CascadeSplits.y) { c = 1u; splitNear = CascadeSplits.x; splitFar = CascadeSplits.y; }
    else if (d < CascadeSplits.z) { c = 2u; splitNear = CascadeSplits.y; splitFar = CascadeSplits.z; }
    else                          { c = 3u; splitNear = CascadeSplits.z; splitFar = CascadeSplits.w; }

    // 基準カスケード c の法線オフセットを両サンプルに使う（帯内で c と c+1 の 4.5cm 段差を無くす）。
    float noff = (1.0f + (float)c) * 0.045f;
    float s = TS_SampleCascade(worldPos, N, c, noff);

    // split 手前 15% の帯で次カスケードへブレンド（継ぎ目消し）。d でブレンド量も回転不変。
    if (c < 3u)
    {
        float band = 0.15f * (splitFar - splitNear);
        float t = (d - (splitFar - band)) / max(band, 1e-4f);
        if (t > 0.0f)
            s = lerp(s, TS_SampleCascade(worldPos, N, c + 1u, noff), saturate(t));
    }
    return s;
}

#endif // TOWN_SHADOW_HLSLI
