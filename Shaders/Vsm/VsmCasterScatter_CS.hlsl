// ============================================================
//  VsmCasterScatter_CS.hlsl — VSM V3c-m2d: count と同一展開で (caster, page) ペアを
//  InstancePairs へ散布。モデル毎に連続領域 [PairBase[m], PairBase[m]+PairCount[m]) に配置。
//  レビュー C1/C2 対策: per-model 上限 (slot<modelEnd) + global cap (slot<gMaxPairs) で
//  越境/オーバーフロー書込みを物理的に封じる。GlobalCounter は総試行数（検証・監視用）。
// ============================================================
#include "VsmBinning.hlsli"

StructuredBuffer<Caster> Casters   : register(t0);   // root SRV
StructuredBuffer<uint>   PageTable : register(t1);   // root SRV
RWStructuredBuffer<uint>  PairBase      : register(u0);
RWStructuredBuffer<uint>  PairCursor    : register(u1);
RWStructuredBuffer<uint>  PairCount     : register(u2);
RWStructuredBuffer<uint2> InstancePairs : register(u3);  // (worldIdx=cid, physPage)
RWStructuredBuffer<uint>  GlobalCounter : register(u4);  // [0]=総試行ペア数
RWStructuredBuffer<uint>  DirtyBounds   : register(u5);  // 空間カリング: dirty領域の光空間AABB(encoded)

float DecF(uint u) { u = (u & 0x80000000u) ? (u & 0x7FFFFFFFu) : ~u; return asfloat(u); }

cbuffer BinCb : register(b1)
{
    uint gCasterCount;
    uint gModelCount;
    uint gPhysCap;
    uint gMaxPairs;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint cid = id.x;
    if (cid >= gCasterCount) return;
    Caster c = Casters[cid];
    uint m = c.meta.x;
    if (m >= gModelCount) return;

    float3 Cl = VsmCasterLS(c);
    float  r  = c.centerRadius.w;
    uint levels = (uint)Vsm_Params.x;
    uint vppr   = (uint)Vsm_Params.z;

    uint modelEnd = PairBase[m] + PairCount[m];   // このモデルの領域上限（C2: 越境禁止）

    for (uint L = 0; L < levels; ++L)
    {
        // レベル毎 空間カリング（count と同一判定でペア数一致を保証）。
        uint bi = L * 4u;
        float bMinX = DecF(DirtyBounds[bi + 0u]), bMinY = DecF(DirtyBounds[bi + 1u]);
        float bMaxX = DecF(DirtyBounds[bi + 2u]), bMaxY = DecF(DirtyBounds[bi + 3u]);
        if (Cl.x + r < bMinX || Cl.x - r > bMaxX || Cl.y + r < bMinY || Cl.y - r > bMaxY) continue;

        int4 rc = VsmLevelRect(Cl, r, L);   // 絶対ページ矩形（窓内）
        for (int ay = rc.y; ay <= rc.w; ++ay)
            for (int ax = rc.x; ax <= rc.z; ++ax)
            {
                uint sx = VsmAbsToSlot(ax, vppr), sy = VsmAbsToSlot(ay, vppr);   // 世界固定スロット
                uint vp = VsmCellVp(L, sx, sy, vppr);
                uint phys = PageTable[vp];
                if (phys != 0xFFFFu && phys < gPhysCap)
                {
                    uint slot; InterlockedAdd(PairCursor[m], 1u, slot);
                    InterlockedAdd(GlobalCounter[0], 1u);
                    if (slot < modelEnd && slot < gMaxPairs)
                        InstancePairs[slot] = uint2(cid, phys);
                }
            }
    }
}
