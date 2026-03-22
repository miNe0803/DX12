# NPR + PBR 同一 HDR での白飛び対策

PBR 背景と NPR キャラを **同じ `HDR_SceneColor`** に描き、**Bloom → ACES トーンマップ**する構成では、肌が Bloom に入りやすく、ACES で高輝度が白に収束しやすい。

## Bloom 抽出（`bloom_extract_ps.hlsl`）

- **閾値を超えた輝度差分だけ**を Bloom に流す（旧: 閾値超えでフルカラー → 肌が丸ごと発光しやすかった）。
- **`bloomKnee`**: 差分に対するソフトニー。大きいほど閾値付近の Bloom が弱くなる（肌向け）。

## エンジン既定（`Scene::InitPostProcess`）

- `threshold` **1.15**、`bloomKnee` **0.55**、`bloomIntensity` **0.38**、`exposure` **0.88**（ACES 前の全体露出）。

## ImGui（`Debug / Async load`）

- **Post process**: Bloom 閾値・強度・露出・ガンマをその場で調整可能。
- （旧）NPR 専用の HDR 倍率・線形クランプは削除済み。白飛びは Bloom 抽出・ポスト露出・NprTonemap／Composite 側で調整する。

## シェーダ

- `bloom_extract_ps.hlsl`: 輝度 vs `threshold`
- `ToneMap_PS.hlsl`: Bloom 加算後に `exposure`、**ACESFilm**、`gamma`
