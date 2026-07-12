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
        int4 rc = VsmLevelRect(Cl, r, L);
        for (int py = rc.y; py <= rc.w; ++py)
            for (int px = rc.x; px <= rc.z; ++px)
            {
                uint vp = VsmCellVp(L, (uint)px, (uint)py, vppr);
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
