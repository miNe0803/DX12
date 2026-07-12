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

// レベル L でキャスタ境界球が覆う仮想ページ矩形(inclusive)。空なら x>z or y>w。
// count/scatter で完全同一の入力→出力（共有関数なので bit-identical）。
int4 VsmLevelRect(float3 Cl, float r, uint L)
{
    float4 lce = Vsm_LevelCenterExtent[L];
    float  E   = lce.z;
    float  vppr = Vsm_Params.z;
    float  pw  = E / vppr;
    float  ox  = lce.x - E * 0.5f;
    float  oy  = lce.y - E * 0.5f;
    int x0 = (int)floor((Cl.x - r - ox) / pw);
    int x1 = (int)floor((Cl.x + r - ox) / pw);
    int y0 = (int)floor((Cl.y - r - oy) / pw);
    int y1 = (int)floor((Cl.y + r - oy) / pw);
    int hi = (int)vppr - 1;
    x0 = max(x0, 0); y0 = max(y0, 0);
    x1 = min(x1, hi); y1 = min(y1, hi);
    return int4(x0, y0, x1, y1);
}

// (L, px, py) → ページテーブル線形index（Vsm_PageTableIndex と一致）
uint VsmCellVp(uint L, uint px, uint py, uint vppr)
{
    return L * (vppr * vppr) + py * vppr + px;
}

#endif // VSM_BINNING_HLSLI
