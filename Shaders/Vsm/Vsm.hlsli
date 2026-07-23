#ifndef VSM_HLSLI
#define VSM_HLSLI
// ============================================================
//  Vsm.hlsli — Virtual Shadow Map アドレッシング（クリップマップ, 光空間）。
//  V2(ページ要求) / V4(サンプリング) が使用。純関数で提供し、CB/リソース束縛は includer 側。
//
//  期待する includer 側の宣言（V4）:
//    cbuffer VsmCB { matrix Vsm_LightView; float4 Vsm_Params; float4 Vsm_ZParams;
//                    float4 Vsm_LevelCenterExtent[8]; };   // = VsmSystem::VsmConstants
//    StructuredBuffer<uint> Vsm_PageTable;   // 仮想→物理index (0xFFFF=無効)
//    Texture2D<float>       Vsm_Atlas;       // 物理深度アトラス
//    SamplerComparisonState Vsm_CmpSmp;      // 比較サンプラ
//
//  Params: x=levelCount, y=pageSize, z=virtualPagesPerRow(vppr), w=atlasPagesPerRow(appr)
//  ZParams: x=lightZNear, y=lightZFar, z=camLightX, w=camLightY
//  LevelCenterExtent[i]: xy=光空間XY中心(スナップ済), z=extent(世界範囲m), w=texelWorld
// ============================================================

// レベル選択: カメラ光空間XYからのチェビシェフ距離が収まる最小レベル。
uint Vsm_SelectLevel(float2 lightXY, float2 camLightXY, uint levelCount, float baseExtent)
{
    float d = max(abs(lightXY.x - camLightXY.x), abs(lightXY.y - camLightXY.y)) * 2.0f;
    uint lvl = (uint)max(0.0f, ceil(log2(max(d / max(baseExtent, 1e-3f), 1.0f))));
    return min(lvl, levelCount - 1u);
}

// ============================================================
//  フットプリント(スクリーン導関数)LOD — UE5級VSMの根本修正 (Phase 1)。
//  距離ベース Vsm_SelectLevel はアイレベル擦過で地面がスクリーンを埋めると、近傍の広い光空間面積へ
//  細レベルを刻印しページ数が爆発する。フットプリントLODは「1スクリーンピクセルが覆う光空間幅 f」を
//  texelWorld(L)>=f となる最小レベルに対応させる（テクスチャmip選択と同じ）。ceil で粗い方向へ丸める
//  ため1画素あたりテクセル数≤1 → 要求ページ総数がスクリーンピクセル数で上限化。擦過/遠方の地面は f が
//  大きく自動的に粗レベルを選ぶので、9216プールに対し数百〜~2000ページに収まる（>4倍余裕）。
//  dLdx/dLdy: 光空間XYのスクリーンX/Y方向偏微分（サンプラ=ddx/ddy, マーカー=隣接画素有限差分）。
//  texelWorld0: level0 の world m/texel（= Vsm_LevelCenterExtent[0].w）。
// ============================================================
uint Vsm_SelectLevelFootprint(float2 dLdx, float2 dLdy, float texelWorld0, uint levelCount)
{
    float f = max(max(abs(dLdx.x), abs(dLdx.y)), max(abs(dLdy.x), abs(dLdy.y)));
    float lvl = ceil(log2(max(f / max(texelWorld0, 1e-6f), 1.0f)));
    return (uint)min(max(lvl, 0.0f), (float)(levelCount - 1u));
}

// 合成: max(距離レベル, フットプリントレベル)。俯瞰では地面がカメラを向くため footprint<=distance となり
// max=distance ＝ 現行と完全一致（構造的な非回帰保証）。アイレベルの擦過地面のみ footprint が距離を上回り、
// 粗レベルへ引き上げてページ数を抑える。マーカー/サンプラ/デバッグは同一式を共有し丸め(ceil)を一致させる。
uint Vsm_SelectLevelCombined(float2 lightXY, float2 camLightXY, uint levelCount, float baseExtent,
                             float2 dLdx, float2 dLdy, float texelWorld0)
{
    uint ld = Vsm_SelectLevel(lightXY, camLightXY, levelCount, baseExtent);
    uint lf = Vsm_SelectLevelFootprint(dLdx, dLdy, texelWorld0, levelCount);
    return max(ld, lf);
}

// V5b 真トロイダル: origin=窓原点(整数ページ座標), pageWorld=1ページの世界幅。
// スロット = 絶対ページ mod vppr（= (origin + 窓内オフセット) & (vppr-1)）。これによりスロットは
// **世界固定**（カメラ移動で origin が変わっても同じ世界ページは同じスロット）＝永続キャッシュが移動でも成立。
// rel は origin を先に引くので大座標でも高精度(レビューF3)。inPageUV は frac(rel)=ページ内位置。
uint2 Vsm_VirtualPage(float2 lightXY, float2 origin, float pageWorld, uint vppr, out float2 inPageUV)
{
    // F3精度対策: origin*pageWorld(≈カメラ光空間)を先にワールド単位で引いてから割る→被除数が小さく高精度。
    float2 rel = (lightXY - origin * pageWorld) / max(pageWorld, 1e-6f);
    inPageUV = frac(rel);
    int2 sw = clamp((int2)floor(rel), 0, (int)vppr - 1);   // 窓内オフセット [0,vppr)
    int2 originI = (int2)origin;                            // 窓原点（整数ページ座標）
    int2 s = (originI + sw) & (int)(vppr - 1u);             // 絶対ページの剰余＝世界固定スロット（負も二の補数で巻く）
    return (uint2)s;
}

// トロイダルスロット(px or py) → 現在窓 [origin, origin+vppr) 内の絶対ページ座標。
// BuildPageParams が「そのスロットが現在保持すべき世界ページ」を復元するのに使う。
int Vsm_SlotToAbsPage(int slot, int origin, uint vppr)
{
    return origin + ((slot - origin) & (int)(vppr - 1u));
}

// ページテーブルの線形index
uint Vsm_PageTableIndex(uint level, uint2 page, uint vppr)
{
    return level * (vppr * vppr) + page.y * vppr + page.x;
}

// 物理ページindex + ページ内UV → 物理アトラスUV
float2 Vsm_PhysicalUV(uint physPage, float2 inPageUV, uint appr)
{
    uint2 tile = uint2(physPage % appr, physPage / appr);
    return (float2(tile) + inPageUV) / (float)appr;
}

// 光空間Z → 正規化深度 [0,1]（比較用）
float Vsm_NormalizeDepth(float lightZ, float zNear, float zFar)
{
    return saturate((lightZ - zNear) / max(zFar - zNear, 1e-3f));
}

// --- V4 で実装する影サンプルの骨子（参考） ---
//  float3 ls = mul(float4(worldPos,1), Vsm_LightView).xyz;           // 光空間
//  uint L = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, (uint)Vsm_Params.x, LEVEL0_EXTENT);
//  float2 uvp; uint2 vp = Vsm_VirtualPage(ls.xy, Vsm_LevelCenterExtent[L].xy,
//                                         Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);
//  uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);
//  uint phys = Vsm_PageTable[idx];
//  if (phys == 0xFFFFu) return 1.0;                                   // 未割当=光
//  float2 auv = Vsm_PhysicalUV(phys, uvp, (uint)Vsm_Params.w);
//  float cmp = Vsm_NormalizeDepth(ls.z, Vsm_ZParams.x, Vsm_ZParams.y) - bias;
//  return Vsm_Atlas.SampleCmpLevelZero(Vsm_CmpSmp, auv, cmp);         // ページ境界PCFは要隣接参照

#endif // VSM_HLSLI
