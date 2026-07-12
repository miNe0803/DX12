// ============================================================
//  VsmAllocate_CS.hlsl — VSM: 要求された仮想ページに物理ページを割当てる。
//  gCacheMode=0: 非キャッシュ版（毎フレーム再割当。V3a）。要求ページに連番物理indexを atomic 発行し
//                ページテーブル(vp→phys)と逆引き(phys→vp)を書く。非要求は 0xFFFF。
//  gCacheMode=1: 永続キャッシュ版（V5b）。静的な町はページが世界固定なので一度描けば永久有効。
//                ・非要求ページ  : PageTable 保持（キャッシュ継続）。
//                ・要求かつresident: 保持（再描画不要）。DirtyPageTable=0xFFFF。
//                ・要求かつ未resident: 永続カウンタから新規physを発行し PageTable/PhysToVirtual に書き、
//                                      DirtyPageTable[vp]=phys で「今フレーム新規＝描画対象」を印。
//                Counter は毎フレームクリアせず高水位を保つ（C++側で cache 時はクリア抑止）。
//                描画は DirtyPageTable を binning に流すことで新規ページのみ（RenderPages はクリア無し、
//                phys→tile が安定＝新規physのタイルは初期化クリア以降未使用でpristine=1.0）。
// ============================================================
StructuredBuffer<uint>   Request        : register(t0);   // vp -> 要求フラグ(V2)
RWStructuredBuffer<uint>  PageTable      : register(u0);  // vp -> phys (0xFFFF=未割当) ※永続（町がサンプル）
RWStructuredBuffer<uint>  PhysToVirtual  : register(u1);  // phys -> vp (描画で射影決定)
RWStructuredBuffer<uint>  Counter        : register(u2);  // [0]=割当数（cache時は高水位）
RWStructuredBuffer<uint>  DirtyPageTable : register(u3);  // vp -> phys（今フレーム新規のみ, 他 0xFFFF）cache専用

cbuffer AllocCb : register(b0)
{
    uint gTotalVirtual;  // = kTotalVirtualPages
    uint gPhysCap;       // = kPhysicalPages
    uint gCacheMode;     // 0=非キャッシュ, 1=永続キャッシュ
    uint _pad;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint idx = id.x;
    if (idx >= gTotalVirtual) return;

    if (gCacheMode == 0u)
    {
        // ---- 非キャッシュ（従来 V3a）----
        if (Request[idx] != 0u)
        {
            uint phys;
            InterlockedAdd(Counter[0], 1u, phys);
            if (phys < gPhysCap) { PageTable[idx] = phys; PhysToVirtual[phys] = idx; }
            else                 { PageTable[idx] = 0xFFFFu; }
        }
        else
        {
            PageTable[idx] = 0xFFFFu;
        }
        return;
    }

    // ---- 永続キャッシュ（V5b）----
    DirtyPageTable[idx] = 0xFFFFu;   // 毎フレーム dirty をクリア（新規のみ後で印）
    if (Request[idx] != 0u)
    {
        uint cur = PageTable[idx];
        if (cur == 0xFFFFu)          // 未resident → 新規割当（永続カウンタ）
        {
            uint phys;
            InterlockedAdd(Counter[0], 1u, phys);
            if (phys < gPhysCap)
            {
                PageTable[idx]      = phys;
                PhysToVirtual[phys] = idx;
                DirtyPageTable[idx] = phys;   // 今フレーム新規＝描画対象
            }
            // 溢れ時は PageTable=0xFFFF のまま（未割当=lit）。次フレーム再挑戦。
        }
        // resident は保持（再描画不要）。
    }
    // 非要求は PageTable を保持（永続キャッシュの本体）。
}
