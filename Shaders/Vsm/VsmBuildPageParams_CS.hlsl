// ============================================================
//  VsmBuildPageParams_CS.hlsl — VSM V3c-m1: 割当済み各物理ページの「描画変換パラメータ」を構築。
//  逆引き PhysToVirtual[phys]→ 仮想ページ(level, px, py) を復号し、そのページを描画する時に VS が
//  ワールド→(そのレベルのライト空間 ortho)→ページ内 → 物理アトラスのタイル(tx,ty) へ配置するのに
//  必要な (レベル中心/範囲, px,py, tx,ty) を出力。V3c-m3 の per-page 描画(ExecuteIndirect)で参照。
// ============================================================
StructuredBuffer<uint>     PhysToVirtual : register(t0);   // phys -> vp
StructuredBuffer<uint>     Counter       : register(t1);   // [0]=割当数
RWStructuredBuffer<float4> PageCenterExtent : register(u0); // per phys: (cx, cy, extent, 0)
RWStructuredBuffer<uint4>  PageTile         : register(u1); // per phys: (px, py, tx, ty)
RWStructuredBuffer<uint>   DirtyPageTable   : register(u2); // vp -> phys（今フレームdirty, 他0xFFFF）
RWStructuredBuffer<uint>   DirtyBounds      : register(u3); // レベル毎 [L*4+0..3]=encMinX,encMinY,encMaxX,encMaxY(光空間)

// VsmSystem::VsmConstants と一致（GetConstantsAddress を b0 にバインド）
cbuffer VsmCB : register(b0)
{
    matrix Vsm_LightView;
    matrix Vsm_InvViewProj;
    float4 Vsm_Params;      // x=levelCount, y=pageSize, z=vppr, w=appr
    float4 Vsm_ZParams;
    float4 Vsm_DepthDim;
    float4 Vsm_LevelCenterExtent[8];
};
cbuffer BuildCb : register(b1) { uint gCacheMode; uint3 _bpad; }

// float を順序保存 uint へ（InterlockedMin/Max 用）。負値も単調。
uint EncF(float f) { uint u = asuint(f); return (u & 0x80000000u) ? ~u : (u | 0x80000000u); }

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint phys = id.x;
    if (phys >= Counter[0]) return;   // 未割当ページはスキップ

    uint vppr   = (uint)Vsm_Params.z;
    uint appr   = (uint)Vsm_Params.w;
    uint levels = (uint)Vsm_Params.x;

    uint vp = PhysToVirtual[phys];
    uint perLevel = vppr * vppr;
    uint level = vp / perLevel;
    uint rem   = vp % perLevel;
    uint py = rem / vppr;   // トロイダルスロット sy
    uint px = rem % vppr;   // トロイダルスロット sx

    // V5b 真トロイダル: スロットは絶対ページの剰余(ap & (vppr-1))。origin=窓原点(整数ページ)から
    // 現在窓 [origin, origin+vppr) 内の絶対ページを復元: ap = origin + ((slot - origin) & (vppr-1))。
    float4 lw = (level < levels) ? Vsm_LevelCenterExtent[level] : Vsm_LevelCenterExtent[0];
    float pw = lw.z;
    int m = (int)vppr - 1;
    int ox_i = (int)lw.x, oy_i = (int)lw.y;
    int apx = ox_i + (((int)px - ox_i) & m);
    int apy = oy_i + (((int)py - oy_i) & m);
    float ox = (float)apx * pw;   // 絶対ページ左端(光空間X)
    float oy = (float)apy * pw;
    PageCenterExtent[phys] = float4(ox, oy, pw, (float)level);   // (ページ左端X, Y, pageWorld, level)
    PageTile[phys] = uint4(px, py, phys % appr, phys / appr);

    // ビニング空間カリング用: このphysが今フレームdirty(=描画対象)なら、その光空間ページ境界を
    // DirtyBounds へ縮約（InterlockedMin/Max）。binning は casters をこのAABBで早期棄却する。
    if (gCacheMode != 0u && DirtyPageTable[vp] == phys)
    {
        uint bi = level * 4u;   // レベル毎AABB（細レベルのdirtyは近傍=タイト→カリング効く）
        InterlockedMin(DirtyBounds[bi + 0u], EncF(ox));
        InterlockedMin(DirtyBounds[bi + 1u], EncF(oy));
        InterlockedMax(DirtyBounds[bi + 2u], EncF(ox + pw));
        InterlockedMax(DirtyBounds[bi + 3u], EncF(oy + pw));
    }
}
