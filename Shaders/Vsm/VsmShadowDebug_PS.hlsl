// ============================================================
//  VsmShadowDebug_PS.hlsl — VSM 影サンプルの検証（フルスクリーン, ToneMap_VS 駆動）。
//  シーン深度→worldPos（MarkPages と同一の Vsm_InvViewProj 復元＝検証済経路）→ライト空間→
//  Vsm.hlsli アドレッシング→物理アトラス深度と比較→影係数を画面出力。
//  正しければ建物/地面に太陽方向の影が整合して現れる（V4 町統合前の決定的検証）。
//  光=明（レベルで淡く色付け）, 影=黒, VSM未被覆=破棄して実描画(CSM影)を透かす, 空=青。
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
cbuffer DbgFlag : register(b1) { uint gVsmFpLod; uint3 _dpad; }   // Phase 1: 本番(TownPS)とロックステップ

Texture2D<float>       SceneDepth    : register(t0);
StructuredBuffer<uint> Vsm_PageTable : register(t1);
Texture2D<float>       Vsm_Atlas     : register(t2);
SamplerState           PointSmp      : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// 1タップ: ライト空間XY(lxy)を完全に再アドレッシング（レベル選択→仮想ページ→ページテーブル→
// 物理UV）して深度比較。スパースアトラスでは PCF の各タップを個別に再アドレッシングするのが正解
// （アトラスUVを直接オフセットすると隣の物理タイル=別ページを踏み無効）。戻り: 1=光,0=影, -1=未割当。
float VsmSampleTap(float2 lxy, float lz, uint L)
{
    float2 uvp;
    uint2 vp = Vsm_VirtualPage(lxy, Vsm_LevelCenterExtent[L].xy,
                               Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);
    uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);
    uint phys = Vsm_PageTable[idx];
    if (phys == 0xFFFFu) return -1.0f;   // 未割当
    float2 auv = Vsm_PhysicalUV(phys, uvp, (uint)Vsm_Params.w);
    float stored = Vsm_Atlas.SampleLevel(PointSmp, auv, 0);
    float mine = Vsm_NormalizeDepth(lz, Vsm_ZParams.x, Vsm_ZParams.y);
    // TownPS と一致（レベル比例バイアス）。旧固定 0.004(=6.4m) は接地影を浮かせていた。
    float bias = (3.0f * Vsm_LevelCenterExtent[L].w) / max(Vsm_ZParams.y - Vsm_ZParams.x, 1e-3f) + 1e-5f;
    return (mine - bias <= stored) ? 1.0f : 0.0f;
}

float4 main(PSInput i) : SV_TARGET
{
    float d = SceneDepth.SampleLevel(PointSmp, i.uv, 0);

    // ★ls とその導関数は空の早期return(=非一様制御フロー)より前に、全画素で評価する（ddx/ddy有効条件）。
    float2 ndc = i.uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 wp = mul(float4(ndc, d, 1.0f), Vsm_InvViewProj);
    float3 P = wp.xyz / wp.w;
    float3 ls = mul(float4(P, 1.0f), Vsm_LightView).xyz;
    float2 dLdx = ddx(ls.xy);
    float2 dLdy = ddy(ls.xy);

    if (d >= 1.0f) return float4(0.35f, 0.45f, 0.7f, 1.0f);   // 空（導関数評価後に返す）

    // レベル（色付け＋PCF半径のワールドスケール用）。Phase 1: フットプリントLOD は本番と同じ式。
    // フルスクリーンPSなので ddx/ddy(ls.xy) がフットプリント（深度復元位置のスクリーン導関数）。
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // V5b: .z=pageWorld
    // 本番サンプラ(TownPS)と同一の連続LOD。距離LODを連続化した効果を frac(lod) で可視化する。
    uint  levels = (uint)Vsm_Params.x;
    float tw0 = Vsm_LevelCenterExtent[0].w;
    float f = max(max(abs(dLdx.x), abs(dLdx.y)), max(abs(dLdy.x), abs(dLdy.y)));
    float lodFp = (gVsmFpLod != 0u) ? log2(max(f / max(tw0, 1e-6f), 1.0f)) : 0.0f;
    float dCheb = max(abs(ls.x - Vsm_ZParams.z), abs(ls.y - Vsm_ZParams.w)) * 2.0f;
    uint  Lc = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, levels, base);
    float edge = saturate((dCheb / max(base, 1e-3f) / exp2((float)Lc) - 0.5f) * 2.0f);
    float lod = clamp(max((float)Lc + edge, lodFp), 0.0f, (float)(levels - 1u));
    uint  L = (uint)floor(lod);
    float fracLod = lod - (float)L;

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
        float s = VsmSampleTap(ls.xy + off, ls.z, L);
        if (s >= 0.0f) { sum += s; wsum += 1.0f; }
    }
    // 診断: VSM未被覆(=CSMフォールバック領域)は暗い青で明示。
    if (wsum < 0.5f) return float4(0.0f, 0.0f, 0.25f, 1.0f);
    float litf = sum / wsum;                                  // 0..1（ソフト）
    // 診断(Proxy A): frac(lod) を hot ramp で表示。距離LOD連続化の効果を静止で検証する。
    //   黒=frac~0（旧: 遠方は全域これ＝整数レベル→移動でレベルが hard フリップ＝チカチカ&リング）。
    //   赤→黄→白=frac 0.5/0.75/1（連続な遷移帯）。遠方の地面に赤〜黄のグラデが現れれば、距離LODが
    //   連続化しトリリニアが遠方でも効いている証拠＝移動時のパチパチ/リングが消えるはず。
    float3 hot = (fracLod < 0.5f)
        ? lerp(float3(0.03f, 0.03f, 0.05f), float3(0.95f, 0.25f, 0.10f), fracLod * 2.0f)
        : lerp(float3(0.95f, 0.25f, 0.10f), float3(1.0f, 1.0f, 0.7f), (fracLod - 0.5f) * 2.0f);
    return float4(hot * lerp(0.55f, 1.0f, litf), 1.0f);
}
