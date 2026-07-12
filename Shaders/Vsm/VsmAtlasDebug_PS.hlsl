// ============================================================
//  VsmAtlasDebug_PS.hlsl — VSM 物理アトラス（深度）をフルスクリーン表示（検証用, ToneMap_VS 駆動）。
//  1.0=空/未描画（暗）、<1.0=キャスタ深度（明）。64×64 のページタイル格子を重ねて配置を確認。
//  m3 のタイル配置/クリップが正しければ、割当タイルだけに構造が出て隣タイルへ滲まない。
// ============================================================
Texture2D<float> Atlas : register(t0);
SamplerState     Smp   : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput i) : SV_TARGET
{
    float d = Atlas.SampleLevel(Smp, i.uv, 0);
    float3 col;
    if (d >= 0.99999f)
        col = float3(0.01f, 0.01f, 0.03f);                 // 空タイル
    else
    {
        float v = saturate(1.0f - d);                      // 近い=明るい
        col = float3(v, 0.2f + 0.6f * v, 0.4f);
    }
    float2 g = frac(i.uv * 64.0f);                          // 64 タイル格子
    if (min(g.x, g.y) < 0.015f) col = lerp(col, float3(1.0f, 0.3f, 0.0f), 0.5f);
    return float4(col, 1.0f);
}
