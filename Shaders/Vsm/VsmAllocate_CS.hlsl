// ============================================================
//  VsmAllocate_CS.hlsl — VSM V3a: 要求された仮想ページに物理ページを割当てる。
//  非キャッシュ版（毎フレーム再割当。キャッシュは V5）。要求ページに連番の物理indexを
//  atomic に発行し、ページテーブル(vp→phys)と逆引き(phys→vp)を書く。V3c がこれで各ページを描画。
// ============================================================
StructuredBuffer<uint>   Request       : register(t0);   // vp -> 要求フラグ(V2)
RWStructuredBuffer<uint>  PageTable      : register(u0);  // vp -> phys (0xFFFF=未割当)
RWStructuredBuffer<uint>  PhysToVirtual  : register(u1);  // phys -> vp (描画で射影決定)
RWStructuredBuffer<uint>  Counter        : register(u2);  // [0]=割当数

cbuffer AllocCb : register(b0)
{
    uint gTotalVirtual;  // = kTotalVirtualPages
    uint gPhysCap;       // = kPhysicalPages
    uint2 _pad;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint idx = id.x;
    if (idx >= gTotalVirtual) return;

    if (Request[idx] != 0u)
    {
        uint phys;
        InterlockedAdd(Counter[0], 1u, phys);
        if (phys < gPhysCap)
        {
            PageTable[idx] = phys;
            PhysToVirtual[phys] = idx;   // V3c: この物理ページが表す仮想ページ
        }
        else
        {
            PageTable[idx] = 0xFFFFu;     // 物理プール溢れ → 未割当（V4 で光扱い）
        }
    }
    else
    {
        PageTable[idx] = 0xFFFFu;
    }
}
