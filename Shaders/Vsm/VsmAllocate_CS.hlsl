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
StructuredBuffer<uint>   Request        : register(t0);   // vp -> 要求(V2)。cache時は bit31=有効|packed絶対ページ
RWStructuredBuffer<uint>  PageTable      : register(u0);  // vp -> phys (0xFFFF=未割当) ※永続（町がサンプル）
RWStructuredBuffer<uint>  PhysToVirtual  : register(u1);  // phys -> vp (描画で射影決定)
RWStructuredBuffer<uint>  Counter        : register(u2);  // [0]=割当数（cache時は高水位）
RWStructuredBuffer<uint>  DirtyPageTable : register(u3);  // vp -> phys（今フレーム新規のみ, 他 0xFFFF）cache専用
RWStructuredBuffer<uint>  ResidentAP     : register(u4);  // vp -> スロット保持中の絶対ページ(packed) cache専用（wrap検出）

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

    // ---- 永続キャッシュ（V5b, 真トロイダル + residentAP + FIFOリング退去）----
    DirtyPageTable[idx] = 0xFFFFu;   // 毎フレーム dirty をクリア（新規/巻いたスロットのみ後で印）
    uint req = Request[idx];         // bit31=有効, 下位=packed絶対ページ
    if (req != 0u)
    {
        uint cur = PageTable[idx];
        // スロットが未resident、または保持中の世界ページ(residentAP)が要求と違う(=カメラ移動で
        // トロイダルスロットが別の世界ページへ巻いた)なら、物理ページを割当てて再描画する。
        if (cur == 0xFFFFu || ResidentAP[idx] != req)
        {
            // FIFOリング: seq を単調加算し phys=seq%cap で循環割当（最古を退去）。プール枯渇しても
            // lit にならず一番古いページを再利用する。作業集合が cap 以下なら可視ページは常駐維持。
            uint seq; InterlockedAdd(Counter[0], 1u, seq);
            uint phys = seq % gPhysCap;
            // 旧所有者を無効化（この物理を今保持している別スロット）。まだPageTableがこの物理を指す時のみ。
            uint oldVp = PhysToVirtual[phys];
            if (oldVp < gTotalVirtual && PageTable[oldVp] == phys)
            {
                PageTable[oldVp]  = 0xFFFFu;   // 退去されたスロットは次に見えれば再要求される
                ResidentAP[oldVp] = 0u;
            }
            PageTable[idx]      = phys;
            PhysToVirtual[phys] = idx;
            ResidentAP[idx]     = req;         // このスロットが今保持する世界ページを記録
            DirtyPageTable[idx] = phys;        // 今フレーム描画対象（+ 再利用タイルは事前クリア）
        }
        // residentAP 一致（同じ世界ページを継続保持）→ 再描画不要。キャッシュ命中。
    }
    // 非要求は PageTable を保持（永続キャッシュの本体）。
}
