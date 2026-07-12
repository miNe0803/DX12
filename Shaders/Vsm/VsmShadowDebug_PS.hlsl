// ============================================================
//  VsmShadowDebug_PS.hlsl — VSM 影サンプルの検証（フルスクリーン, ToneMap_VS 駆動）。
//  シーン深度→worldPos（MarkPages と同一の Vsm_InvViewProj 復元＝検証済経路）→ライト空間→
//  Vsm.hlsli アドレッシング→物理アトラス深度と比較→影係数を画面出力。
//  正しければ建物/地面に太陽方向の影が整合して現れる（V4 町統合前の決定的検証）。
//  光=明（レベルで淡く色付け）, 影=黒, 未割当ページ=マゼンタ, 空=青。
// ============================================================
#include "Vsm.hlsli"

cbuffer VsmCB : register(b0)
{
    matrix Vsm_LightView;
    matrix Vsm_InvViewProj;
    float4 Vsm_Params;       // x=levelCount, y=pageSize, z=vppr, w=appr
    float4 Vsm_ZParams;      // x=zNear, y=zFar, z=camLightX, w=camLightY
    float4 Vsm_DepthDim;
    float4 Vsm_LevelCenterExtent[8];
};

Texture2D<float>       SceneDepth    : register(t0);
StructuredBuffer<uint> Vsm_PageTable : register(t1);
Texture2D<float>       Vsm_Atlas     : register(t2);
SamplerState           PointSmp      : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// 1タップ: ライト空間XY(lxy)を完全に再アドレッシング（レベル選択→仮想ページ→ページテーブル→
// 物理UV）して深度比較。スパースアトラスでは PCF の各タップを個別に再アドレッシングするのが正解
// （アトラスUVを直接オフセットすると隣の物理タイル=別ページを踏み無効）。戻り: 1=光,0=影, -1=未割当。
float VsmSampleTap(float2 lxy, float lz)
{
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // V5b: .z=pageWorld
    uint L = Vsm_SelectLevel(lxy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base);
    float2 uvp;
    uint2 vp = Vsm_VirtualPage(lxy, Vsm_LevelCenterExtent[L].xy,
                               Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);
    uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);
    uint phys = Vsm_PageTable[idx];
    if (phys == 0xFFFFu) return -1.0f;   // 未割当
    float2 auv = Vsm_PhysicalUV(phys, uvp, (uint)Vsm_Params.w);
    float stored = Vsm_Atlas.SampleLevel(PointSmp, auv, 0);
    float mine = Vsm_NormalizeDepth(lz, Vsm_ZParams.x, Vsm_ZParams.y);
    float bias = 0.004f;                 // ≈6.4m world（ZParams 1600m 圧縮下）。要調整
    return (mine - bias <= stored) ? 1.0f : 0.0f;
}

float4 main(PSInput i) : SV_TARGET
{
    float d = SceneDepth.SampleLevel(PointSmp, i.uv, 0);
    if (d >= 1.0f) return float4(0.35f, 0.45f, 0.7f, 1.0f);   // 空

    float2 ndc = i.uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 wp = mul(float4(ndc, d, 1.0f), Vsm_InvViewProj);
    float3 P = wp.xyz / wp.w;
    float3 ls = mul(float4(P, 1.0f), Vsm_LightView).xyz;

    // レベル（色付け＋PCF半径のワールドスケール用）
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // V5b: .z=pageWorld
    uint L = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base);

    // PCF: 選択レベルのテクセル世界サイズを半径に、8タップ Vogel をライト空間XYへオフセット。
    // 各タップを完全再アドレッシング（スパース安全）。未割当タップ(-1)は光扱いで無視。
    float texelW = Vsm_LevelCenterExtent[L].w;   // world m / texel（そのレベル）
    float radius = texelW * 1.5f;                // ペナンブラ幅（テクセル単位）
    const int TAPS = 8;
    float sum = 0.0f; float wsum = 0.0f;
    [unroll] for (int t = 0; t < TAPS; ++t)
    {
        float ang = (t + 0.5f) * (6.2831853f / TAPS);
        float r = sqrt((t + 0.5f) / TAPS) * radius;
        float2 off = float2(cos(ang), sin(ang)) * r;
        float s = VsmSampleTap(ls.xy + off, ls.z);
        if (s >= 0.0f) { sum += s; wsum += 1.0f; }
    }
    if (wsum < 0.5f) return float4(1.0f, 0.0f, 1.0f, 1.0f);   // 全タップ未割当=マゼンタ
    float litf = sum / wsum;                                  // 0..1（ソフト）
    float lit = lerp(0.25f, 1.0f, litf);                      // 影0.25〜光1.0

    float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.55f, 0.75f, 1.0f), (float)L / 7.0f);
    return float4(lit * tint, 1.0f);
}
