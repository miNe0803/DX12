# DX12 → UE5「Downtown_West」昼間シーン マッチング ロードマップ

UE5プロジェクト `C:\Unreal\TEST`（Downtown_West サンプル）を7領域で解析し、DX12ポートとのギャップを洗い出したもの。

## 1. 現状 vs UE5（各領域の最大ギャップ）

| 領域 | UE5 | DX12 現状 | 最大ギャップ |
|---|---|---|---|
| Lighting | 動的太陽+SkyLight+Point/Spot、街灯は昼間emissive | CSM太陽+静的IBL、ClusterGrid未使用 | 昼間はほぼ差なし（Forward+は夜間用の負債）|
| Post-Process | histogramオート露出+ASC-CDLグレード+local exposure+filmic ACES | exposure=1.0固定、グレード/WB/ビネット無し | **オート露出が無い**（明暗が根本的に合わない）|
| Atmosphere/Fog | ExponentialHeightFog+Volumetric、SkySphere HDRI | 単ローブheight fog、scatteringG=0.7 | fog実数値未取得+単ローブで昼ヘイズ不一致 |
| Shadows/GI | VSM(全レベル)+Lumen GI+SSAO | **CSM cascade0のみ/512px**、GIは静的IBLのみ | **~38m以遠が影ゼロ**、動的GI/AO無し |
| Decals | DBuffer(Albedo+Normal+Roughness合成) | albedoのみ、純Lambert | 法線/ラフネス無くヒビ/濡れが平面的 |
| Materials | 2セットブレンド+頂点ペイント+detail+puddle+wind | 単一セット、頂点色破棄、**rough読めずflat0.8**、葉静止 | 別ラフネス未サンプル+木が完全静止 |
| Reflections | Lumen反射(実シーン)+box/sphereプローブ | 静的skyキューブ+平面puddleのみ | 光沢面が**街でなく空だけ**を映す |

## 2. 優先ステップ（impact/effort比順、各ステップ独立リリース可能）

### Phase A: 効果大・低コスト（すぐ効く）
- **A1** 別ラフネスマップをサンプル（現状flat0.8→素材ごとの光沢差）— `TownScene.cpp`/`TownPS.hlsl` — S
- **A2** fog実数値をUEエディタから吸い出し（以降の調整の前提データ）— `AtmosphereSystem.h` — S
- **A3** トーンマッパ Narkowicz→ACESFitted(Hill行列版) — `ToneMap_PS.hlsl` — S/M
- **A4** 暫定ambient GI（空色tint+ambientBoost+AOゲート）— `TownPS.hlsl`/`StandardPBR_PS.hlsl` — S
- **A5** volumetric位相/温度是正（scatteringG 0.7→~0.2、temporal ON）— `AtmosphereSystem.h`/`Volumetric_CS.hlsl` — S
- **A6** Post小物（white balance / vignette / chromatic aberration）— `ToneMap_PS.hlsl` — S
- **A7** IBLアンビエントが昼exrから正しく生成されているか確認 — `Scene.cpp` — S
- **A8** Town UVタイリング+albedoバリエーション — `TownPS.hlsl` — S

### Phase B: 中規模
- **B1** ★CSMを4カスケード全部使う（~38m以遠に影＝シーン全体が別物）— `Scene.cpp`/両PS — M
- **B2** ★葉の風(WPO)（完全静止の木が動く）— `TownVS.hlsl` — M
- **B3** カラーグレーディング(ASC-CDL+彩度)（色相/コントラスト/彩度が揃う）— `ToneMap_PS.hlsl` — M
- **B4** ★オート露出/eye adaptation（明暗基準がUEと一致）— 新規`LumHistogram_CS.hlsl`+`PostProcessSystem` — L
- **B5** Decalに法線+ラフネス/スペキュラ+RGBマスク分岐 — `TownDecalPS.hlsl`/`TownDeferredDecalPS.hlsl` — M/L
- **B6** Town detail(マクロ)層（近接テクセル反復解消）— `TownPS.hlsl` — M
- **B7** height-fog色モデル拡張(StartDistance/MaxOpacity/inscatter分離)— `FogComposite_PS.hlsl` — M
- **B8** SSAO/GTAOパス（接地影・くぼみの陰り）— 新規SSAOパス — M
- **B9** Bloomマルチスケール化（広く映画的な光）— `PostProcessSystem.cpp` — M
- **B10** 影解像度512→2048+回転Poisson PCF+bias（ギザギザ解消）— `ShadowSystem.h` — S/M
- **B11** ガラス屈折+per-materialマップ — `TownPS.hlsl`(glass) — M
- **B12** 葉のSSS/両面透過（逆光で葉が透ける）— `TownFoliagePS.hlsl` — M
- **B13** 地面puddle/wetness(頂点色前提)— `TownPS.hlsl`(ground) — M
- **B14** 空の太陽ディスク同期 — `Skybox_PS.hlsl` — M
- **B15** Local exposure近似 — `ToneMap_PS.hlsl` — L

### Phase C: 大規模／近似（Lumen・VSM等の代替）
- **C1** ★汎用SSR(HiZマーチ)＝Lumen反射近似（街が街を映す）— `HiZSystem`+新規`SsrTrace` — L
- **C2** ★SSGI単バウンス＝Lumen GI近似（間接光/色被り）— 新規 — L
- **C3** 反射プローブ系(parallax-correct、UEのcapture位置流用)— 新規 — XL
- **C4** 2セットブレンド+頂点ペイント(puddleの前提)— `TownVS`/`TownPS` — L
- **C5** 反射フォールバック階層(SSR→プローブ→sky cube)— 新規resolve — M
- **C6** DBuffer decalエミュ(multiply blendで路面を暗く)— decal PSO — L
- **C7** モーションブラー — 新規velocityパス — L
- **C8** Forward+ローカルライト有効化（主に夜間用）— `ClusterGrid` — L

## 3. リテラル移植 **不可能** な機能 → 推奨近似

| UE5機能 | 不可の理由 | 推奨近似 |
|---|---|---|
| **Lumen GI** | SW/HWRT+distance fieldベース | SSGI単バウンス(C2)＋暫定sky-tint ambient(A4) |
| **Lumen Reflections** | 同上トレース基盤前提 | 汎用SSR(C1)+反射プローブ(C3)+skyキューブ階層fallback(C5) |
| **Virtual Shadow Maps** | 仮想化ページアトラス | 多カスケードCSMを実際に全部使う(B1)+2048+良質PCF(B10) |
| **Ray Tracing** | DXRパス休眠中 | screen-space(SSR/SSGI/SSAO)で代替。休眠DXR水面はhero用に任意接続 |
| **Local Exposure** | histogram tonemap組込み | ブラーluma近似(B15) |
| **filmic ACES** | RRT/ODT+gamut圧縮が重い | ACESFitted(Hill行列)(A3) |

## 4. あえて「やらない」こと（過剰マッチの罠）
- **SkyAtmosphere(Rayleigh/Mie)/VolumetricCloudを足さない**。参照mapは両方未使用のレガシーHDRI SkySphere。物理散乱を足すと逆に乖離。DX12のexponential height fogが正しいアナログ。
- 昼間の街灯はemissive prop（Lumenでbounce）でありper-lamp動的ライトではない。DX12もemissive扱いで正解。Forward+動的ローカルライト(C8)は夜間用に後回し。

## 5. 推奨進行
Phase Aを1本ずつマージ（特にA1ラフネス/A2 fog実数値/A3トーンマッパは即効）。次にPhase Bの**B1カスケード修正**と**B4オート露出**が二大幹（明暗と影の範囲がUEに寄る）。Phase CはSSR(C1)→SSGI(C2)→反射プローブ(C3)の順でLumen相当を段階的に積み上げる。
