#ifndef VSM_BINNING_HLSLI
#define VSM_BINNING_HLSLI
// ============================================================
//  VsmBinning.hlsli — VSM V3c-m2 の共有アドレッシング。
//  count(m2b) と scatter(m2d) が**同一の**ページ展開関数を include して使うことで、
//  両パスの被覆ページ集合を bit-identical に保証（レビュー C2: FP 非決定性で境界破壊するのを防ぐ）。
//
//  規約: ls = mul(float4(worldPos,1), Vsm_LightView)（CB 転置格納）。
//  レベル L の世界範囲 extent=4*2^L、pageWorld=extent/vppr、レベルは
//  [center-extent/2, center+extent/2] を覆う（Vsm.hlsli:Vsm_VirtualPage と一致）。
// ============================================================

// VsmSystem::VsmConstants と一致（b0）
cbuffer VsmCB : register(b0)
{
    matrix Vsm_LightView;
    matrix Vsm_InvViewProj;
    float4 Vsm_Params;              // x=levelCount, y=pageSize, z=vppr, w=appr
    float4 Vsm_ZParams;             // x=zNear, y=zFar, z=camLightX, w=camLightY
    float4 Vsm_DepthDim;
    float4 Vsm_LevelCenterExtent[8];// xy=中心, z=extent, w=texelWorld
};

// キャスタレコード（TownScene::CasterRec と一致, 96B）
struct Caster
{
    float4x4 world;        // transpose(BuildLocal*G)（描画 VS 用）
    float4   centerRadius; // xyz=ワールド中心, w=ワールド半径（スケール込み, レビュー H1）
    uint4    meta;         // x=modelId
};

// キャスタ中心をライト空間へ
float3 VsmCasterLS(Caster c)
{
    return mul(float4(c.centerRadius.xyz, 1.0f), Vsm_LightView).xyz;
}

// レベル L でキャスタ境界球が覆う「絶対ページ矩形」を現在窓 [origin, origin+vppr) に切って返す(inclusive)。
// 空なら x>z or y>w。真トロイダル: 返すのは絶対ページ座標。count/scatter が各絶対ページを
// スロット=(ap & (vppr-1)) へ写す（世界固定スロット）。count/scatter で完全同一の入力→出力。
int4 VsmLevelRect(float3 Cl, float r, uint L)
{
    // V5b: lce=(originX,originY,pageWorld,texel)。origin=窓原点(整数ページ)。
    float4 lw = Vsm_LevelCenterExtent[L];
    float  pw = lw.z;
    int    vppr = (int)Vsm_Params.z;
    int ox = (int)lw.x, oy = (int)lw.y;
    int ax0 = (int)floor((Cl.x - r) / pw);
    int ax1 = (int)floor((Cl.x + r) / pw);
    int ay0 = (int)floor((Cl.y - r) / pw);
    int ay1 = (int)floor((Cl.y + r) / pw);
    // 現在窓 [origin, origin+vppr) に交差クランプ（窓外は非常駐なので不要）。
    ax0 = max(ax0, ox); ay0 = max(ay0, oy);
    ax1 = min(ax1, ox + vppr - 1); ay1 = min(ay1, oy + vppr - 1);
    return int4(ax0, ay0, ax1, ay1);   // 絶対ページ座標（inclusive, 窓内）
}

// (L, スロットpx, スロットpy) → ページテーブル線形index（Vsm_PageTableIndex と一致）
uint VsmCellVp(uint L, uint px, uint py, uint vppr)
{
    return L * (vppr * vppr) + py * vppr + px;
}

// 絶対ページ座標 → 世界固定トロイダルスロット（Vsm_VirtualPage と一致: ap & (vppr-1)）
uint VsmAbsToSlot(int absPage, uint vppr)
{
    return (uint)(absPage & (int)(vppr - 1u));
}

#endif // VSM_BINNING_HLSLI
