# VSM V3c (GPU駆動ページ描画) 実装設計 — 敵対レビュー反映版

> 設計ワークフロー wf_0bd9cc53-310（Understand×4 → Design → 敵対Review×3）の成果。
> 実装前に3人の専門レビュアーが致命的問題を検出済み。以下は**修正反映後**の確定設計。

## 方式
thread-per-caster ページ展開 → モデル単位 prefix-sum → scatter → per-submesh 間接引数 →
単一 `ExecuteIndirect` で 8192² 物理アトラスへ深度描画（VSがタイル配置、`SV_ClipDistance`でタイルクリップ）。
per-page カリング(pages×casters=8.6M)を避け O(Σ重なりページ) へ削減。

## ⚠️ 最重要の検証結果（feasibility レビュー）
**頂点増幅で naive 実装は TDR する。** 各(caster,page)ペアがサブメッシュ全頂点を再変換し、
粗レベルは町全域を覆うため全ビルが全ページへフル解像度で再描画 → 10^8〜10^9 VS/frame。
→ **ページキャッシュ（ダーティページのみ再描画）は「最適化」でなく「実現可能性の前提」。**
UE5がVSMを成立させている本質はNaniteのper-page LOD + キャッシュ。本エンジンにNaniteは無い。
**対策（必須）**: (a) 毎フレーム全描画しない＝クリップマップ中心が動いた時/キャスタが動いた時のみ再描画、
(b) ハードcap + オーバーフロー時フレームskip、(c) 粗レベルでの小サブメッシュ間引き(LOD)。
初期検証は縮退構成（キャスタ絞る/粗レベルのみ）でTDRを避けて数値検証→段階拡張。

## 致命的修正（レビュー指摘、実装済みにすること）
- **C1**: m2e で `InstanceCount = (base>=kMaxPairs)?0:min(PairCount[m], kMaxPairs-base)`。VS側で `worldIdx>=casterCount || phys>=allocCount` を退場(w=0)ガード。
- **C2**: 展開ロジックは共有 `VsmBinning.hlsli` の関数で count/scatter 完全一致。scatter は per-model ローカル上限 `slot < PairBase[m]+PairCount[m]` を強制（境界FPズレでも越境不可）。
- **C3**: 全リソースの「フレーム終端状態＝翌開始状態」表を固定。`BuildPageParams`冒頭で pageCenterExtent/pageTile を NON_PIXEL→UAV 復帰、pageTable を UAV 復帰、InstancePairs/VsmDrawArgs を UAV 復帰。
- **H1/MED-5**: `CasterRecords.centerRadius` は `inst.worldCenter`/`inst.worldRadius`（スケール込み・算出済み）を格納。model ローカル半径を使わない。
- **H2**: VS の ndcZ は saturate せずクリップ（`ls.z<zNear`/`>zFar` を clip distance で退場）。範囲外キャスタの偽影防止。
- **H3**: count/scatter で `if (c.meta.x >= gModelCount) return;`。CPUで modelCount<=kMaxModels(512) を assert。
- **H4**: scatter で `uint phys=PageTable[vp]; if (phys>=kPhysicalPages) continue;`。
- **HIGH-4(perf)**: 大キャスタが級レベル全域(64×64)にクランプ→PageTable読み爆発。実測して必要なら level丸ごと高速経路 or 反復面積上限。
- **MED bias**: SlopeScaledDepthBias はレベル間で texelWorld が128倍差→アクネ不揃い。受光側 normal-offset を主に。

## m2 コンピュート段（すべて numthreads(64,1,1)）
- reset: PairCount/GlobalPairCounter を 0（zero copy）
- **m2b VsmCasterCount_CS**: caster→light空間中心/半径→各レベル extent 矩形→割当ページ(PageTable≠0xFFFF)を数え `InterlockedAdd(PairCount[modelId], n)`
- **m2c VsmPrefixSum_CS**(dispatch 1): exclusive prefix → PairBase[m], PairCursor[m]=PairBase[m], total
- **m2d VsmCasterScatter_CS**: 同一展開(共有hlsli)→`slot=InterlockedAdd(PairCursor[m],1)`; per-model+global cap内で `InstancePairs[slot]=(cid,phys)`
- **m2e VsmBuildDrawArgs_CS**: submeshBatch毎に DRAW_INDEXED_ARGS 生成（InstanceCount=cap後PairCount[m], StartInstance=PairBase[m]）

## m3 描画段
- 単一 8192² DSV(常時 DEPTH_WRITE)＋単一ビューポート＋`SV_ClipDistance`×4でタイル矩形クリップ（半texel内締め）
- 新規: `VsmCasterDepth_VS`(slot0 POSITION only + slot1 PER_INSTANCE uint2)、深度専用PSO、CommandSignature(DRAW_INDEXED, stride20, rootsig=null)
- VSタイル配置: world→(c.world)→ls=(・Vsm_LightView)→page内local→atlas UV(サンプル側 inPageUV と厳密一致, Y反転)→NDC
- 統合 VB/IB（ユニークモデル連結）+ SubmeshGeoTable（indexCount/startIndex/baseVertex/modelId）

## 新規リソース
TownScene(load時): CasterRecords(96B: world転置64+centerRadius16+meta16), SubmeshGeoTable(16B), 統合VB/IB。
VsmSystem(毎frame): PairCount/PairBase/PairCursor(uint×512), InstancePairs(uint2×kMaxPairs,~1M推奨/8MB), GlobalPairCounter(uint), VsmDrawArgs(20B×submeshBatch)。

## 実装順（小分け・各段readback検証）
1. **m2-setup**: CasterRecords + SubmeshGeoTable + modelId採番（TownScene）。readbackでレコード/行列照合。
2. **m2b-e binning**: 4 CS。各段readback（ΣPairCount==GlobalPairCounter, worldIdx/phys範囲, DrawArgs整合）。**純compute=TDRなし**。実ペア数を実測。
3. **m3a**: 1ページCPU描画でVS配置数式検証（Y反転/深度符号/転置）。
4. **m3b/c**: clip distance + 全アトラス + ExecuteIndirect化。**ハードcap+skip-on-overflow必須**。
5. **可視化**: アトラスをデバッグ表示。
6. **キャッシュ**: 中心不動時skip（TDR回避の前提）。
7. **V4**: TownShadow.hlsli / TownPS のサンプラを Vsm.hlsli アトラス参照へ差替＝初の可視。
