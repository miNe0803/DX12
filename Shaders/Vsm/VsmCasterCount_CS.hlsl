// ============================================================
//  VsmCasterCount_CS.hlsl — VSM V3c-m2b: 各キャスタが覆う「割当済みページ」数を
//  モデル別に集計。thread-per-caster。scatter(m2d)と同一の VsmLevelRect を使う。
//  出力 PairCount[modelId] += n。
// ============================================================
#include "VsmBinning.hlsli"

StructuredBuffer<Caster> Casters   : register(t0);   // root SRV
StructuredBuffer<uint>   PageTable : register(t1);   // root SRV (vp -> phys, 0xFFFF=未割当)
RWStructuredBuffer<uint> PairCount : register(u0);   // root UAV (per model)
RWStructuredBuffer<uint> DirtyBounds : register(u1); // 空間カリング: dirty領域の光空間AABB(encoded)

float DecF(uint u) { u = (u & 0x80000000u) ? (u & 0x7FFFFFFFu) : ~u; return asfloat(u); }

cbuffer BinCb : register(b1)
{
    uint gCasterCount;
    uint gModelCount;
    uint gPhysCap;      // = kPhysicalPages
    uint gMaxPairs;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint cid = id.x;
    if (cid >= gCasterCount) return;
    Caster c = Casters[cid];
    uint m = c.meta.x;
    if (m >= gModelCount) return;                 // レビュー H3: OOB atomic 防止

    float3 Cl = VsmCasterLS(c);
    float  r  = c.centerRadius.w;
    uint levels = (uint)Vsm_Params.x;
    uint vppr   = (uint)Vsm_Params.z;

    uint n = 0;
    for (uint L = 0; L < levels; ++L)
    {
        // レベル毎 空間カリング: このキャスタの光空間AABBが「このレベルのdirty領域AABB」と重ならなければ
        // このレベルのセル走査をスキップ（細レベルはdirtyが近傍でタイト→大半のキャスタを棄却）。
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
                if (phys != 0xFFFFu && phys < gPhysCap) ++n;   // レビュー H4
            }
    }
    if (n > 0) InterlockedAdd(PairCount[m], n);
}
