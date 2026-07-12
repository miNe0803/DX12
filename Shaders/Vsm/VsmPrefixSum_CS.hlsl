// ============================================================
//  VsmPrefixSum_CS.hlsl — VSM V3c-m2c: モデル別 PairCount の exclusive prefix-sum。
//  PairBase[m]=Σ_{k<m} PairCount[k]、PairCursor[m]=PairBase[m]（scatter の書込みカーソル初期値）、
//  Totals[0]=総ペア数。gModelCount(<=512-1024) は小さいので単スレッド直列で十分・競合なし。
// ============================================================
RWStructuredBuffer<uint> PairCount  : register(u0);
RWStructuredBuffer<uint> PairBase   : register(u1);
RWStructuredBuffer<uint> PairCursor : register(u2);
RWStructuredBuffer<uint> Totals     : register(u3);   // [0]=total pairs

cbuffer BinCb : register(b0)
{
    uint gCasterCount;
    uint gModelCount;
    uint gPhysCap;
    uint gMaxPairs;
};

[numthreads(1, 1, 1)]
void main()
{
    uint acc = 0;
    for (uint m = 0; m < gModelCount; ++m)
    {
        PairBase[m]   = acc;
        PairCursor[m] = acc;
        acc += PairCount[m];
    }
    Totals[0] = acc;
}
