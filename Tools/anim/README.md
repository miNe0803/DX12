# Animation retarget pipeline (UE/Kawaii FBX → MMD hibana → .skcl)

Blender ヘッドレスで UE Mannequin / Kawaii の FBX アニメを hibana.pmx(MMD) へリターゲットし、
DX12 エンジンが再生する独自バイナリ `.skcl` を書き出す一式。

エンジン側の再生・状態機械・スプリングボーンは `Systems/CharacterAnimator.{h,cpp}`。
生成物（`assets/Animations/clips/**` の `.skcl`, ~2GB）と元FBX（`Characters/`,`KawaiiAnimations/`,`RepresentativeWithMesh/`）は
巨大なため `.gitignore` 済み。**この tools/anim だけで再生成できる**ようにするための保管。

## 前提
- Blender 5.0（`C:\Program Files\Blender Foundation\Blender 5.0\blender.exe`）＋ 拡張 **mmd_tools**（blender.org版, PMX読込）
- `assets/hibana/hibana.pmx`、`assets/Animations/**` の元FBX（ローカルに存在すること）

## 座標ブリッジ（検証済み定数, `bridge_C.npy`）
Blender armature空間→エンジンmodel空間の相似変換 C（一様スケール12.5＝1/0.08 ＋ −90°X回転）。
399骨の関節位置対応で最小二乗、残差0.05mm。列ベクトル系。詳細は memory `skinning-retarget-pipeline`。

## ファイル
| script | 役割 |
|---|---|
| `dump_blender_bones.py` | hibana の全骨名/rest を JSON 出力（ブリッジ導出用） |
| `bridge_solve2.py` | エンジン骨ダンプ＋Blender骨から座標ブリッジ C を解いて `bridge_C.npy` 保存 |
| `export_rig.py` | `springbones.rig`（髪/スカート等の spring チェーン定義）を出力 |
| `batch_all_grouped.py` | **全FBXを種類別に一括変換** → `assets/Animations/clips/groups/<GROUP>/<name>.skcl` |
| `batch_export.py` | 主要クリップをフラット出力（コア用, idle/walk/run 等） |
| `physics_batch.py` | （旧）MMD剛体物理をベイクして .skcl 化。※現在はランタイムSpringBoneのためコアでもFK版で可 |
| `export_clip.py` / `stage4_contact.py` | 単体変換 / 検証レンダ |
| `bridge_C.npy`, `engine_bones.txt` | 導出済みブリッジ＋エンジン(Assimp)骨名+offset ダンプ（exporterが読む） |

## ⚠️ パス注意
各スクリプト冒頭の `SCR = r"...\scratchpad"` は開発時の一時パスを指す。再実行時は
`SCR` を **この tools/anim フォルダ**（`bridge_C.npy`, `engine_bones.txt` の場所）に書き換えるか、
両ファイルをそのパスへ置く。

## 再生成手順（フル）
1. エンジンを `DX12_DUMPBONES=<path>\engine_bones.txt` で一度起動し hibana 骨+offset をダンプ。
2. `blender -b --python dump_blender_bones.py -- <out.json>`
3. `blender -b --python bridge_solve2.py`（`bridge_C.npy` 生成, 残差<1mm を確認）
4. `blender -b --python export_rig.py`（`springbones.rig`）
5. `blender -b --python batch_all_grouped.py`（全クリップ→ groups/）

## エンジン側
`CharacterAnimator::LoadClipLibrary("assets/Animations/clips", scale)` がコア(.skcl)を常駐ロード、
`LoadGroup("<GROUP>")` で種類をオンデマンドロード。`springbones.rig` は `LoadSpringRig` が読む。
