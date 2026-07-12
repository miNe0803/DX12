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

// レベル内の仮想ページ座標 + ページ内UV。center/extent は該当レベルのもの。
uint2 Vsm_VirtualPage(float2 lightXY, float2 center, float extent, uint vppr, out float2 inPageUV)
{
    float2 local = (lightXY - center) / max(extent, 1e-3f) + 0.5f;   // [0,1]
    local = saturate(local);
    float2 pf = local * (float)vppr;
    uint2 page = (uint2)min(pf, (float)(vppr - 1u));
    inPageUV = frac(pf);
    return page;
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
