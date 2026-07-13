// ============================================================
//  VsmClearTiles_VS.hlsl — VSM V5b: 永続キャッシュの FIFO リング退去で「再利用される物理タイル」は
//  前の世界ページの深度が残る(stale)。キャスタ描画は幾何のある画素しか書かないため、空き画素に
//  古い深度が残ると誤った影になる。そこで dirty（今フレーム(再)割当された）タイルだけを深度1.0(遠)へ
//  事前クリアする。インスタンス = 仮想ページ(vp) 全数、DirtyPageTable[vp] が有効な物理のみタイル矩形を
//  z=1.0 で描く（非dirty はクリップ）。PSO は depth-write ON / DepthFunc ALWAYS（無条件上書き）。
//  この後に RenderPages が LESS 比較でキャスタ深度を書き込む＝正しい最近深度が残る。
// ============================================================
RWStructuredBuffer<uint> DirtyPageTable : register(u0);   // vp -> phys（今フレーム dirty のみ, 他 0xFFFF）

cbuffer ClearCb : register(b0)
{
    uint gAppr;       // アトラス1行のページ数（= kAtlasPagesPerRow）
    uint gPageSize;   // 1ページの解像度(texel)
    uint gAtlasDim;   // アトラス全体の1辺(texel) = appr*pageSize
    uint gTotalVp;    // = kTotalVirtualPages（インスタンス上限）
};

float4 main(uint vid : SV_VertexID, uint iid : SV_InstanceID) : SV_Position
{
    if (iid >= gTotalVp) return float4(2.0f, 2.0f, 2.0f, 1.0f);      // 範囲外 → クリップ
    uint phys = DirtyPageTable[iid];
    if (phys == 0xFFFFu) return float4(2.0f, 2.0f, 2.0f, 1.0f);      // 非dirty → クリップ（NDC外）

    uint tx = phys % gAppr;
    uint ty = phys / gAppr;
    float2 c = float2((float)(vid & 1u), (float)((vid >> 1) & 1u));  // tri-strip 角: (0,0)(1,0)(0,1)(1,1)
    float px = ((float)tx + c.x) * (float)gPageSize;                // アトラス画素座標
    float py = ((float)ty + c.y) * (float)gPageSize;
    float2 ndc = float2(px / (float)gAtlasDim * 2.0f - 1.0f,
                        1.0f - py / (float)gAtlasDim * 2.0f);
    return float4(ndc, 1.0f, 1.0f);   // z=1.0（遠＝影の初期値）
}
