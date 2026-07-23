# DXR ディフューズGI 実装計画（静的フォワード町シーン向け）

> 独立3案（reuse-first per-pixel RayQuery / DDGIワールドプローブ / ReSTIR）→ 敵対レビュー → 統合
> のワークフロー(2026-07-24)で確定。ゴール: フラットな偽ambientを実バウンスGIに置換。

## 1. ゴール

現状の間接光は偽物（sky-tint + ambientBoost + スクリーン空間GTAO）で、VSM影下・軒下・裏側がフラット、バウンス光も色のにじみも無い。これがUE5lookとの最大の残ギャップ。DXR 1.1 で**静的な町**に時間的に安定したディフューズ間接光を供給し、TownPSのフラットなambient項を実データに置換する。**フォワード（G-bufferなし）・TAAなし**の制約を破綻なく満たすことを最優先。

## 2. 採用アーキテクチャ（ブレンド）

**GIペイロード = DDGI（ワールド空間 irradiance プローブ・ボリューム）。実装基盤 = inline RayQuery（compute内）＋AS修復。ReSTIRは初回GIには過剰・不採用（将来のフィデリティ強化として文書化のみ）。**

なぜ静的町+フォワード+TAAなしに最適か:
- **TAAなし問題の唯一クリーンな解**: DDGIの時間フィルタは*ワールド空間*（プローブごとEMAヒステリシス）。スクリーン再投影・モーションベクトル不要、ディスオクルージョン免疫。RTXGI由来で静的町では数フレームで収束後完全静止。スクリーン空間GI（提案1/3）は1-2spp を自作デノイザで支える必要＝最大の破綻リスク（boiling/ghosting）。
- **フォワード+G-bufferなし問題の最クリーン適合**: DDGIサンプルはスクリーン法線不要。TownPSは既に `worldPos` と法線マップ後 `Nw`（TownPS.hlsl:363）を持ち `(worldPos,Nw)` で直接サンプル → **GIパスに新規RTゼロ**。深度再構成法線が要るのはRTAOだけ（AOは低周波で許容）。
- **静的町が理想ケース**: TLASはロード時1回構築（FAST_TRACE）、プローブ収束後静止。DDGIの弱点（鋭い接触バウンスが出ない）は先行実装のRTAOが接触遮蔽で補完。
- 光漏れ（薄壁貫通）は Chebyshev可視性 + プローブ再配置 + 法線バイアスで対処（**必須**）。

## 3. フェーズ

### Phase F1 — TLAS基盤（最低リスク・独立出荷可能）〈L 約1-1.5週〉
休眠 RayTracingManager のAS部を修復し、静的町（建物/地面/プロップ、フォリッジ除外）を BLAS+TLAS 登録。inline RayQuery CS でヒット距離デバッグビュー。町の見た目は無変更。
- `RayTracingManager.{h,cpp}`: `AddBLAS`(RTM.cpp:126)を**複数ジオメトリ=1モデル1BLAS**化（TLASインスタンス≈アクタ数~4000-5000）。**【全提案が見落とした致命傷】scratchバッファ永久保持(RTM.cpp:165)を構築後に解放**（数百BLASでVRAM破綻）。`BuildTLAS`(RTM.cpp:183)をper-frame FAST_BUILD→**1回きりFAST_TRACE**。`GeometryInfo` StructuredBuffer新設(vbSrvIdx/ibSrvIdx/baseColorSrvIdx/stride/indexBase)。頂点Position offset0・**stride 84**（提案2の44は誤り）。`CreateRTPipeline`/shader-table/`DispatchWaterReflection`(RTM.cpp:264死コード)は**隔離**（DispatchRays経路は使わない）。
- `TownScene.{h,cpp}`: 不足アクセサ追加（サブメッシュ `ID3D12Resource*` VB/IB、インスタンス `{modelId->blasIndex, worldT}`）。**VSMキャスタ集合をASインスタンスの単一真実源**に。`isFoliage/isTree`除外（不透明クアッド＝黒ハロー）。
- `Core/Scene.cpp`: Gtao初期化付近でRTManager生成、町ロード後TLAS1回構築。コマンドリストを `ID3D12GraphicsCommandList4` へ QueryInterface して成功検証。
- 新規 `Shaders/RT/RtDebugPrimary_CS.hlsl`: primary rayヒット距離をUAVへ。
- **ゲート**: PIXで TLASインスタンス数≈アクタ数。ヒット距離ヒートマップが深度と1px以内一致。OFFで町描画バイト一致、VSM/GTAO無影響。AS構築メモリ/時間ログ。

### Phase R — RTAO（レイトレース環境遮蔽）〈L 約1.5-2週〉
TLASから接触遮蔽を供給。半解像度で1-2本のコサイン半球オクルージョンレイ(TMax≈2-3m)、時間再投影+バイラテラルアップサンプル、HDRへ乗算。既存GTAOはOFF時フォールバック温存。**唯一のスクリーン空間時間再投影をここで構築**しinline RayQuery/ASをGI前にde-risk。
- 新規 `RtaoSystem.{h,cpp}`: GtaoSystem構造をクローン（半解像度R16 UAV、深度SRV、`DEPTH_WRITE↔NON_PIXEL_SRV`バリア列 GtaoSystem.cpp:186-213、`ToneMap_VS`+DEST_COLOR/ZERO乗算合成 GtaoSystem.cpp:114-131）。
- 新規 `Shaders/RT/Rtao_CS.hlsl`: 深度→ワールド位置、ddx/ddyクロスで法線再構成、TLASへ短レイ、blue-noise+フレームジッタ。
- 新規 `Shaders/RT/RtaoDenoise_CS.hlsl`: 深度+法線ガイドのエッジ保存バイラテラル（半→全アップサンプル兼用）+ **静的町の厳密再投影による時間累積**。
- `Core/Scene.cpp`: **prev深度コピー(R32) + prev-ViewProj 保持**（現状なし）。Gtaoが計算する InvViewProj/cameraPos(Scene.cpp:1761)流用。GTAOと同スロット(Scene.cpp:2706)。
- **ゲート**: GTAO横並びで軒下/車両下の遮蔽がタイト・正確（深度ハローなし）。静止で時間安定、移動でゴーストなし。半解像度で≤2ms@1080p目安。OFFで現行画像厳密再現、VSM無回帰。

### Phase G — DDGI ディフューズGI（単一カスケード）※本命 〈XL 約3-4週〉
プローブボリューム構築（八面体irradiance + depth/visibilityアトラス）、RTでプローブ更新、TownPSのambient項へ注入しフラットambientBoostを置換。カメラ中心1カスケード、ヒットのアルベドはモデル定数tintから開始。
- 新規 `DdgiSystem.{h,cpp}`: irradiance(RGB16F)/depth(RG16F)アトラス、グリッドCB、再配置オフセットaux。
- 新規 `Shaders/RT/ProbeTrace_CS.hlsl`: inline RayQuery。ヒット=太陽*RTサン影レイ、ミス=空、+**前フレームプローブirradianceを無限バウンス加算**。ラウンドロビン更新。
- 新規 `Shaders/RT/ProbeBlend_CS.hlsl`: 八面体irradiance/depth 時間ヒステリシス(EMA ~0.95-0.98)+ボーダーコピー+**プローブ再配置(必須)**。
- 新規 `Shaders/RT/DdgiOct.hlsli`: 八面体エンコード + **Chebyshev/分散可視性重み+backface除去+法線バイアス(薄壁光漏れ対策・必須)**。
- 新規: `Vsm.hlsli` に**微分不要のレベル選択ヘルパ**（TownPSのfootprint-LODはddx/ddy依存でCS/レイ文脈で無効。サン影レイのヒットシェーディングに必須。既存純アドレッシング関数は再利用）。
- `TownPS.hlsl`: ambientブロック(TownPS.hlsl:415-418)で `diffuseIBL*ambientBoost` を `プローブirradiance(worldPos,Nw)*albedo*kD` に置換。specular IBL温存、RTAO乗算。新規irradiance/depth SRV + グリッドCB + `gGI`フラグ。**OFF時は現行ambient経路厳密維持**。レジスタマップ(t9-t12のVSM/SSR)衝突を事前監査。
- **ゲート**: 測定可能な色のにじみ。VSM影下/裏側にバウンスフィル。**壁貫通の光漏れなし**。TAAゼロで時間安定・数フレームで収束。`gGI` OFFで現行ambient厳密一致。VSM/性能無回帰。

### Phase G2 — マルチカスケード + フィデリティ（任意・逐次）〈L 2週+〉
単一グリッドを~3段ネストカスケード（2m/8m/32m間隔）へ拡張、間隔スナップでクロール防止、町全体~1.6kmカバー。任意でヒットのアルベドを定数→テクスチャ化（GeometryInfo+bindlessでVB/IB/UVフェッチ、barycentric補間）。VSMクリップマップのセンタリング概念流用。

## 4. 二大難問の解決

**(A) フォワードでのピクセル法線**: GIペイロード(DDGI)は法線RT不要（採用の決め手）。TownPSの既存 `Nw`+`worldPos` で直接サンプル、**新規RTゼロ**。RTAOのみ深度再構成法線（GTAO同一、低周波で許容）。アップグレード（品質ゲート不合格時のみ）: `Nw` を第2MRTへ書出し（全メインパスPSOにRTV追加＝侵襲的なので必要が証明されるまで着手しない）。

**(B) TAAなしデノイズ**: GI(DDGI)は**デノイズ不要**（時間フィルタがワールド空間プローブEMA、再投影・モーションベクトル不要、ディスオクルージョン免疫）。RTAOは**自己完結の1再投影**（現深度→prev-VP投影→prev深度で検証、静的ジオメトリなのでカメラのみ再投影は幾何学的に厳密）。

## 5. 休眠 RayTracingManager: 再利用 vs 再構築
- **再利用**: `AddBLAS`/`BuildTLAS` コア、DXRサポートチェック。
- **修復**: 複数ジオメトリBLAS / **scratch解放(VRAMリーク修正)** / TLAS 1回FAST_TRACE / GeometryInfo新設 / CommandList4 QI。
- **新規**: inline RayQuery用の新bindlessコンピュートルートシグ、RtaoSystem/DdgiSystem、全RTコンピュートシェーダ、prev深度+prev-VP、Vsm.hlsli微分不要レベル選択、TownPS注入。
- **隔離(死コード)**: `CreateRTPipeline`/shader-table/`DispatchWaterReflection`/`WaterReflection_RayGen.hlsl`/既存グローバルルートシグ(非bindless)。

## 6. スコープ
現実的 **6-9週**（1名、調整済Phase Gまで）。F1 ~1-1.5週 / R ~1.5-2週 / G ~3-4週 / G2 ~2週+（スライス可）。全フェーズ既定OFFフラグ裏で独立出荷可能、OFF時バイト一致、VSM無回帰。

## 7. 最重要の未決定事項
**「拡散的だが安定なDDGIフィル」を最終到達点とするか、DDGIを安定ベースラインとして上に鋭い接触GI（per-pixel 1バウンスRayQuery / ReSTIR）を後続フェーズでロードマップ入りさせるか。**
DDGIは構造的に**低周波の拡散フィルのみ**（鋭い接触バウンス・視点依存の鋭いにじみは出ない、RTAOが接触遮蔽で部分補完）。「UE5 Lumen相当の鋭い間接光」が目標ならDDGIを土台にper-pixel GIを積む前提でF1のGeometryInfo/テクスチャ付きヒットを厚めに設計。「フラット影下を破綻なく自然に持ち上げる」が目標ならDDGI単独(+RTAO)で十分、per-pixel/ReSTIRは無期限延期可。この判断がG2以降の投資規模とF1のGeometryInfo作り込み度を左右する。
