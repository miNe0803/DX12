
===== review:addressing =====
検証完了。設計書の数式を実コード（`Vsm.hlsli` / `VsmSystem.h`: `kBaseExtent=4.0m`, `vppr=appr=64`, `kPageSize=128`, `kLevels=8`, `phys=4096`, `SelectLevel`はチェビシェフ距離×2ベース）と突き合わせた。

---

# VSM V5b addressing 敵対的レビュー

## 是認できる部分（穴なし）

- **負 apX の `& (VPPR-1)`（§1.2）**: 正しい。HLSL `int&` は32bit二の補数ビット演算。`apX=-1 → 0xFFFFFFFF&63=63 = (-1 mod 64)`。任意の負 apX で数学的 mod と一致。`(uint)`キャストも AND 後が [0,63] なので安全。
- **`desiredAPX`（§2.2）**: 正しい。`originX + ((sx-originX)&63)` は `[originX,originX+64)` 内で `≡sx (mod64)` の唯一値を返す。検証: `originX=60, sx=3 → 60+((3-60)&63)=60+7=67`（67 mod64=3 ✓）、`originX=-5,sx=3 → -5+8=3 ✓`。
- **窓内 1:1（§1.3）**: 連続64整数が mod64 の完全剰余系 → 全単射。**ただし「可視点の apX が窓内」が前提**（下記 F1/F2 で崩れる）。
- **free-list atomic pop/push（§2.4）**: 正しい。`InterlockedAdd(-1)`の戻り `old` が各スレッド一意 → `old>0` を見たスレッドのみ distinct index を pop、他は復旧+1。全 pop の Pass2 内では net = initial - min(initial,threads)。一時的 underflow（0xFFFF..）は内部的で、`FreeCount` をサイズとして読む並行コードがなければ無害。2パス+UAVバリアで push/pop 分離も妥当。
- **KEEP/REUSE/RELEASE 状態機械（§2.3）**: 論理は健全。world-locked ゆえ AP 一致で KEEP、スクロール列のみ REUSE(dirty)。
- **§5 の「インテリア不変」機序**: 中心相対 vs 絶対の対比は正しく、大域揺れが消えるのは事実。

---

## 重大な破綻・穴

### F1. ガードバンド 31·pw は f→1 で余裕ゼロ → 遠端ストリップの誤ページ（揺れ残存）

窓は `[c-32, c+31]`（c=floor(camLX/pw)）。カメラ光空間の小数位置を `camLX=(c+f)pw, f∈[0,1)` とする。可視ピクセル最大オフセット maxΔ の点の `apX = floor(c+f+maxΔ/pw)`。

設計のガード `d=2·maxAxis·64/62` は `maxAxis ≤ 31·pw`（ceil ゆえ等号あり）しか保証しない。すると窓右端 `(c+32)pw` までの余裕は

```
margin = (c+32)pw - (camLX+maxΔ) = (32 - f - maxΔ/pw)·pw ≥ (1 - f)·pw  →  f→1 で 0
```

**反例**: カメラが自ページの上端付近（f=0.98）、レベル L の最遠可視片が maxAxis=31·pw。厳密算術では apX=c+31 で窓内だが、余裕は 0.02·pw ＝ サブテクセル。ここに **F2 の CPU/GPU 浮動小数ズレ**が乗ると `floor` が c+32 に飛ぶ。

**結果**: `apX=c+32` の可視片は `sx=(c+32)&63`。reconcile は同 sx に対し窓内で `desired = desiredAPX(sx, c-32) = c-32`（∵ c+32 ≡ c-32 mod64）。つまり**64ページ離れた反対端のワールドページ**を保持する phys を町がサンプル → その深度で遮蔽判定 → 遠端に幅1ページの誤影ストリップ。カメラが境界を跨ぐ度に f が 0↔1 を往復 → **ストリップが点滅（＝揺れが局所的に残存）**。設計の「フリンジ1フレーム遅延のみ」では説明できない、恒常的な誤りである。

**修正**: ガードを 2ページに。`maxAxis ≤ 30·pw` を強制（factor `VPPR/(VPPR-4)=64/60`）すれば `margin ≥ (2-f)pw > 1pw` を常時確保、サブテクセルの浮動小数ズレを吸収できる。設計の 64/62（→31pw）は f→1 域で不足。

### F2. 町の apX（GPU）と窓 originX（CPU）が別経路 → 境界での off-by-one

設計は「整数 originX を CB で渡し float-floor 差異を排除」（§1.6）と主張するが、**originX は reconcile/desiredAPX にしか効かない**。町（TownPS §1.5）は `apX = floor(ls.x/pw)` を **GPU で独立に**計算し、それで vp を引く。町・MarkPages は互いに一致するが（同じ GPU 式）、**caster が書く内容は reconcile の desired（CPU originX 由来）**。両者が一致するのは「apX が窓内」の時だけ。

- CPU: `floorf(camLX/pw)`（camLX=ZParams.zw）
- GPU: `floor(ls.x/pw)`（ls = worldPos × Vsm_LightView、行列積の別 float 経路）

camLX が丁度ページ境界 `k·pw` 近傍のとき、`camLX/pw` が CPU で k、GPU で k-ε→k-1 と割れると、窓とページ格子が丸ごと1ページずれ、**エッジ1列が毎フレーム誤 vp を読む**。カメラは境界を絶えず横切るのでチラつく。F1 と合わさって顕在化する。

**修正**: 町の apX 計算を caster と同じ「大整数原点減算後に割る」経路へ寄せる（F3 と共通）。少なくとも境界判定を CPU/GPU 一貫の量（例えば originX を渡し `apX-originX` を GPU で組む）に統一し、`floor` を1回だけにする。

### F3. 絶対インデックスの float32 精度が原点距離で劣化 — 町だけ regression（caster は無傷）

`pw_0 = 4/64 = 0.0625m`、level0 テクセル = 0.49mm。町は `lx/pw` を**そのまま**割る（`ls.x/0.0625 = 16·ls.x`）。float32 の ULP は `|lx/pw|·2^-23`。

- ページ単位で1テクセル = 1/128 = 0.0078。
- `|lx/pw| ≈ 128000`（=光空間 8000m）で ULP ≈ 0.015 ページ単位 > テクセル → **`floor(lx/pw)` の off-by-one と `frac` の量子化がテクセルより粗くなる**。

**反例**: 町が光空間原点から数 km（Vsm_LightView が回転主体で並進が小さい＝町の world 座標がそのまま光空間座標になる配置）に置かれると、level0 の影が原点距離に比例してスイム/スナップする。

**非対称性が致命的**: caster VS は `(ls.x - ox)/pw`（ox=apX·pw を先に引く → 被除数 < pw、小さい）で**高精度**。町は大きい `lx/pw`。**同一ワールド点で描画と抽出の apX/inPageUV が食い違う**。現行の相対方式（`(lightXY-center)/extent`、center=カメラで小さい）は精度を保っていたので、V5b の絶対化は町サンプル側の精度**後退**である。

**修正**: 町も `lx_frac = (ls.x - originX·pw)/pw`（大整数原点を先に減算）で inPageUV を組み、apX は `originX + (int)floor(lx_frac + ...)` 的に小さい相対量から復元する。あるいは Vsm_LightView をカメラ近傍に周期リベースし絶対光座標を小さく保つ。設計はこの精度課題に無言で、絶対アドレッシング固有のリスクを見落としている。

### F4. `Requested` のフレームクリアが未規定 → プール枯渇リーク

reconcile Pass1 は `req==0` で RELEASE する（§2.3）。しかし §2.1「永続バッファはクリアしない」と併記され、**Requested を毎フレームクリアする記述がどこにもない**。クリアされないと `Requested[vp]` が1のまま残り、視界外に出た vp が永遠に KEEP → phys が返却されず `FreeCount→0`（CAP HIT）→ 新規ページ確保不能 → 前進すると影が付かなくなる。

**修正**: MarkPages 前に `Requested` を全 0 クリア（または reconcile が読了後に 0 を書く）を明示。段階検証にも「移動継続で FreeCount が単調減少しないこと」を追加すべき。

### F5. 最粗レベル被覆 = 半径 ~248m（ガード後）— 遠景で強制 L7 が窓外

`kLevels=8` 固定、`extent_L=4·2^L`。最粗 L7 の窓半幅 = `32·pw_7 = 32·(512/64) = 256m`、ガード後の可視保証は **248m**。`SelectLevel` は `min(lvl, levelCount-1)` で L7 にクランプ（Vsm.hlsli:24）するので、**光空間で 248m を超える可視片は強制 L7 かつ窓外** → F1 と同じ機序で64ページ折り返しの誤ページ or 未常駐（影なし）。

**反例**: 見通しの良い町の遠景（数百 m 先の建物と長い太陽シャドウ）で、遠方地形/建物の影が消えるか誤って出る。町スケール次第では実害。**修正/確認**: 必要シャドウ距離 ≤ 248m を実測確認、超えるなら kLevels を増やす（レイアウト 32768→拡張）か baseExtent を上げる。設計は 8 レベルの被覆限界を明記していない。

---

## 中程度の指摘

### M1. 「1フレームフリンジ」の厚みは速度依存（§5 の過小評価）

フリンジ幅 = フレーム間のページスクロール数 = カメラ速度/pw。高速 WSAD（特に低レベルの小 pw）では前進端の**帯全体**が1フレーム古い/未常駐になり、「細いフリンジ」ではなく**方向性のあるラグ・スミア**として1フレーム見える。大域揺れではないが「フリンジ1列のみ」という記述は誇小。速度上限やヒステリシスの想定を明記すべき。

### M2. 窓の非対称 `[c-32, c+31]` の前方被覆が狭い側

`originX=floor-32` により自ページは窓内 index 32、前方は31ページ・後方32ページ。カメラは通常前方を見るので、**被覆の狭い31側が視線方向**に来る。ガード（F1）は31側基準なので設計と整合するが、前方視錐台が後方より深いという典型状況で前方 margin が先に枯れる。F1 の 2ページ化と併せて評価を。

### M3. FreePush のオーバーフロー無防備

`FreeList[4096]`。invariant `free+alloc=4096` が保たれる限り `i≤4095` だが、二重解放バグ1件で `i≥4096` → 隣接 UAV 破壊。検証CS（§追加）で `FreeCount∈[0,4096]` と `PhysOwner↔SlotDesc` 二重所有=0 を assert する計画は妥当だが、push 側に `if(i<PHYS_CAP)` ガード（MarkDirty 同様）を入れておくのが安全。

---

## 総括

- **アドレッシングの代数（mod マスク・desiredAPX・剰余系1:1・free-list）は正しい。** これらは是認。
- **破綻は「可視点 apX が窓内」という単一前提の縁で集中発生する**: F1（ガード 31pw が f→1 で余裕0）、F2（町 GPU-floor と CPU-originX の境界不一致）、F3（絶対座標の float32 精度が町側だけ劣化）、F5（L7 クランプで 248m 超が窓外）。いずれも「64ページ折り返しの誤ページを町がサンプル」という同一故障に収束し、**設計が謳う「揺れ消滅」は遠端・境界・遠景で恒常的に破れる**。
- **F4（Requested 未クリア）は機能停止（プール枯渇）に直結する仕様抜け。**

**最優先修正**: (1) ガードを2ページ（`maxAxis≤30·pw`）に、(2) 町の apX/inPageUV を caster と同じ大整数原点減算経路へ統一（F2+F3 同時解消）、(3) Requested の毎フレームクリアを明記、(4) 248m 被覆で足りるかシーン実測。これらを入れない限り Stage 1 の「WSAD 揺れ消滅」検証はインテリアでは通るが、境界横断・遠景で回帰する。

参照ファイル: `C:\Users\siuts\source\repos\DX12\Shaders\Vsm\Vsm.hlsli`（現行 SelectLevel/VirtualPage）、`C:\Users\siuts\source\repos\DX12\Source\Graphics\VsmSystem.h`（定数 baseExtent=4m, levels=8, vppr=appr=64, phys=4096）。

===== review:cache-race =====
以下、V5b設計を cache-race 観点で敵対的に精査した。深刻度順。妥当な部分は末尾に簡潔に是認する。

---

## CRITICAL-1: KEEP が LightView（太陽）変化に盲目 — staleness検出の致命的抜け

**機序**: KEEP判定は `s.residentAP == desired` の**ライト空間ページindex一致のみ**。だが phys タイルの中身は「あるフレームの `Vsm_LightView` で投影・正規化された深度」であり、LightView が変われば同一ワールド点の `(lx,ly,lz)` すべてが変わる。residentAP は「今の LightView 基準の index」で desired と比較されるが、**格納深度は過去の LightView で焼かれたもの**。両者は独立に stale になりうる。

**破綻シナリオ（昼夜サイクル / 太陽ゆっくり回転）**:
- フレーム毎に `LightView` が微小変化。camLX（ライト空間カメラX）も動くが、あるレベル L で `originX(L)` が**このフレームはページ境界を跨がない**とする。
- → 全 vp が `residentAP==desired` で KEEP → `DirtyCount==0` → §3.2 の count buffer 駆動で**クリア0 draw・キャスタ0 draw = ゼロ再描画**。
- しかし光線方向（深度軸）は変わっている → 全常駐タイルの深度が誤り → **影が太陽に追従せず「貼り付き」、originX がやっと1列跨いだ瞬間にその列だけ更新されて残りが取り残される → 影がスナップする**。

**設計内の自己矛盾**: §6 は「`lightView` の memcmp を NeedsRender で維持」と書くが、これは VSM 丸ごと skip の可否を決めるだけ。太陽が動けば NeedsRender は「レンダせよ」と言う。だが reconcile に入っても全 KEEP で `DirtyCount==0` → **「レンダせよ」と言われたのに何も描かない**パスが成立する。NeedsRender と reconcile のダーティ判定が非結合。

**修正案**: LightView（および projection の zNear/zFar）が前フレームと異なる場合、reconcile 冒頭で**全 SlotDesc を無効化（valid=0）または global dirty epoch をインクリメントして全 phys を強制ダーティ**にする。「太陽不変」を KEEP の必要条件に昇格させること。residentAP 一致だけを KEEP 条件にしてはならない。

---

## CRITICAL-2: `Requested[vp]` のクリア規定が欠落 → free-list 恒久枯渇

**機序**: MarkPages は `InterlockedOr(Requested[vp],1)`。reconcile Pass1 は `req==0` を解放トリガにする。だが §2.1 の「フレーム跨ぎでクリアしない永続バッファ」表に `Requested` は載っていない一方、本文にどこにも「毎フレーム Requested をクリアする」と明記がない。

**破綻シナリオ**: もし Requested を永続扱い（またはクリア漏れ）にすると、一度可視になった vp は `req==1` のまま二度と 0 に戻らない → **解放パスが永久に発火しない** → 探索を続けるほど FreeCount が単調減少 → 数百フレームで `FreeCount==0`（恒久 CAP HIT）→ 新規ページが一切確保できず影が欠落。しかも「揺れ・速度」テストは短時間で通ってしまうため**検出が遅れる**。

**修正案**: `Requested` を明示的に per-frame 一掃（MarkPages 前に UAV clear、または reconcile Pass1 末尾で `Requested[vp]=0`）と設計書に明記。永続バッファ表と transient バッファを厳密に分離し、どの CS がいつゼロクリアするか責務を書くこと。

---

## HIGH-3: 4096超過時、KEEP が枠を握り続け新規が餓死 → 恒久ホール + 順序依存フリッカ

**機序**: §2.4 に「KEEP/REUSE は free-list に触れない」。つまり**可視で resident のページは絶対に枠を返さない**。Pass2 の新規 alloc だけが枯渇の影響を受ける。

**破綻シナリオ（グレージング視点で複数レベルの窓が合算 >4096）**:
1. FreeCount が 0 に張り付く。
2. 既存 KEEP ページ（古い側・粗レベル含む）は永久に居座る。
3. **新規に視界へ入った領域は毎フレーム FreePop==INVALID → PageTable=INVALID → 影なし**。しかも budget を超えている間これが**解消されない = 恒久的な影の穴**。設計は「グレースフル劣化・退行なし」と言うが、現行の全再割当なら少なくとも「今見えている所」を優先確保できたのに対し、V5b は**過去の resident を優先し現在の可視を犠牲にする**ので、体感はむしろ悪化しうる。
4. さらに Pass2 の alloc 勝者は FreePop / スレッドスケジュール順依存 → **どの新規ページが枠を得るかがフレーム毎に非決定 → シマー/フリッカ**。

**修正案**: 枯渇は「病的」で済ませず、(a) 最低限フレーム間で決定的な優先度（レベル昇順・距離順など安定キー）で alloc 順を固定してフリッカを消す、(b) §2.5 で後回しにした「粗レベル KEEP を犠牲にする LRU/優先度エビクション」は V5b 必須に格上げ検討。少なくとも「現在可視 > 過去 resident」の逆転が起きない設計にすること。

---

## HIGH-4: 高速移動時のフリンジは「1フレーム遅延」ではなく「速度幅ぶんの誤ワールドページ」

**機序**: §5 は「新規に窓へ入った縁のみ1フレーム遅延」と主張。だが町(フレームN)は N-1 末構築の PageTable を読む。カメラが速く、1フレームで `originX` が **k>1 ページ**ぶん動くと：
- 新規 k 列の各 vp スロットは、N-1 では「k ページ前の絶対ページ」を保持していた。
- 町 N はその vp を読む → **欠落ではなく、k ページ離れた実在の別ワールド領域の深度**を読む。
- → フリンジ幅 = k = ページ/フレーム（速度比例）。中身は「未常駐」ではなく**誤った実深度** → 移動方向の先端/後端に**偽の影が帯状に出る/揺れ残る**。

「1フレーム遅延のみ」という記述は幅と内容誤りを過小評価している。WSAD スプリント + 高レベル（pw 大）ほど顕著。

**修正案**: フリンジを「未常駐(INVALID)」に落として偽影を出さない方が安全（誤深度より無影の方が目立たない）。具体的には reconcile で「今フレーム desired と residentAP が不一致だが REUSE でまだ再描画前」の状態を町に対して INVALID として見せる（PageTable を N の reconcile 後に更新し、REUSE で MarkDirty したページは描画完了まで PageTable=INVALID にしておき、翌フレーム確定してから有効化する二段公開）。または predictive に camera 速度ぶん窓を先読みして prefetch。

---

## MEDIUM-5: SelectLevel の距離メトリックと窓原点の不整合 → 窓外可視ページが誤スロットへエイリアス

**機序**: §1.4 の窓は `originX=floor(camLX/pw)-VPPR/2`。SelectLevel は「カメラ相対距離 `d=2·maxΔ`」ベースで、§1.4 では camLX に `ZParams.zw` の**非スナップ生値**、窓は §1.6 で `camLX`。両者が同一値である保証・同一 floor である保証が明記されていない。ガードバンドは `d` を `VPPR/(VPPR-2)` 倍する**ヒューリスティックなスケール**であり、ハードクランプではない。

**破綻シナリオ**: あるレベル遷移帯・画面端で、可視ピクセルの実 `apX = originX-1`（窓外1ページ）を選んでしまうと、その `vp` の slot は `sx=apX&63` に対応するが、その slot の desiredAP（窓基準）は `apX+VPPR`（別ワールドページ）。→ MarkPages はその vp を Requested に立て、reconcile はその slot を desiredAP(=別ページ)で確保/KEEP、**町はその slot を自分の apX 期待で読む → 剰余系衝突で別ワールドページの影が出る**。レベル境界の細い帯に誤影・揺れが残る。

**修正案**: SelectLevel と窓原点で**同一の camLX・同一 floor 式**を共有する（CB に確定した originX を配り、SelectLevel も `apX∈[originX,originX+VPPR)` に入るレベルを選ぶ or 窓外になる apX を持つピクセルはそのレベルを不採用にする hard guard）。ヒューリスティックな `d` スケールではなく、`apX` の窓内クランプで縁ケースを構造的に排除すること。

---

## MEDIUM-6: `DirtyCount` の無条件インクリメント vs guarded list-write + count buffer の state 遷移未規定

**機序**: `MarkDirty`: `InterlockedAdd(DirtyCount,1,i); if(i<PHYS_CAP) DirtyList[i]=phys;` — **count は無条件で増え、list書き込みだけ guard**。現状の解析では dirty phys は最大 4096 で境界内に収まる（各 phys は高々1回 MarkDirty)ので今は破綻しない。だが count と list の一貫性が崩れる設計は脆い：将来 MarkDirty を per-vp（32768 可能性）で呼ぶ改修が入れば `DirtyCount>4096` → ExecuteIndirect の instance count に使うと **DirtyList[4096..] の未初期化領域を読む → 乱れた phys でクリアクワッドが描かれ、KEEP タイルを含む無関係タイルを 1.0 で潰す → 生きている影が消える**。

加えて §3.2 は「`DirtyCount` を count buffer に使う」と言うが、**reconcile が UAV で書いた DirtyCount/ペア数を `INDIRECT_ARGUMENT` state へ遷移させるバリア、および UAV→indirect の可視性**が明記されていない。遷移漏れは「古い count で draw」「count 未確定で draw」= 描画欠落 or 過剰。クリア用(DirtyCount)とキャスタ用(ペア数)で**別 count buffer が2本要る**点も明示なし。

**修正案**: count も guarded `i` を使う（`InterlockedAdd`後に上限で飽和、または CAP 到達で明示エラーフラグ）。count buffer は reconcile 後に UAV バリア → COPY/遷移 → `ExecuteIndirect`。2本の count buffer と各バリアを設計書に明記。

---

## MEDIUM-7: 再利用タイル境界テクセルの取り残し / タイル跨ぎフィルタでの stale

**機序**: REUSE/Alloc は MarkDirty→クリアクワッド(128×128, ALWAYS)+キャスタ(LESS_EQUAL) で被覆されるので中身自体は刷新される。だがクリアクワッドが**タイル矩形をテクセル完全被覆する保証**（ラスタライズ規則・ハーフテクセルオフセット・`phys%appr` タイル配置の整数性）が §4 に明記されない。境界1テクセルでもクリア漏れがあれば、そのテクセルは**旧ワールドページの深度を保持** → スクロール毎に境界にシャドウの継ぎ目/フリッカ。

さらに VSM サンプリングがタイル境界を越えてフィルタ（PCF/線形）する場合、隣接タイルは**別ワールドページ**であり、再利用でその隣接が別 apX に切り替わると stale/不連続が滲む。現行が per-page クランプ済みなら是認だが、設計書はこの不変条件に触れていない。

**修正案**: クリアクワッドはタイルより数テクセル大きく（またはビューポート/シザーでタイルにハードクランプ）確実に全被覆。町サンプルは inPageUV を [0,1] で per-page クランプしタイル跨ぎフィルタを禁止する（現行踏襲なら明記）。

---

## LOW / 脆弱性: free-list の underflow 復旧は「push→barrier→pop」厳守下でのみ安全

**解析結果（是認寄り）**: FreePop の `InterlockedAdd(-1)` → `old>0` 判定 → 失敗時 `+1` 復旧は、**Pass1 で push 完結・UAV バリア・Pass2 で pop のみ**という規律下では二重確保も負数残留も起きない（`old>0` を返す回数 ≤ 初期在庫で一意、失敗 pop は `old≤0` なので index 読み出しに至らず復旧は count を正に押し上げない）。ここは正しい。

**しかし警告**: §2.3 末尾・§4 の最適化提案「reconcile 2パスを統合 / BuildPageParams を reconcile へ統合」を実行すると、**push と pop が同一 dispatch で交錯**する。その瞬間、復旧 `+1` が別スレッドの pop に「正の old」を供給しうる経路が開き、**同一 FreeList index の二重確保（同じ phys を2つの vp が所有）→ PhysOwner 上書き → 二重解放 → OOB**に直行する。設計書は統合を「推奨最適化」と書くが、free-list 復旧ロジックはこの統合と**両立しない**。加えて `FreePush` は `FreeList[i]=phys` に**上限チェックがない**（二重 free で i≥4096 になれば OOB write）。

**修正案**: 「2パス分離は free-list 正当性の前提条件であり最適化で崩してはならない」と設計書に赤字で固定。統合したいなら CAS ベースの lock-free stack か per-frame の非交錯保証に作り直す。FreePush にも `if(i<PHYS_CAP)` を入れ、超過は二重解放バグの検出点にする。

---

## その他の指摘

- **動的キャスタの無効化なし**: 中身が world-locked で「静止シーンなら」有効という前提。動くオブジェクト/揺れる木/開くドアがあると KEEP タイルは更新されず影が固まる。町が完全静的なら是認だが、前提として明記すべき。
- **init CS の順序**: FreeCount=4096/FreeList=[0..4095]/全 SlotDesc.valid=0/PhysOwner=INVALID の初期化と、1フレーム目の count buffer 初期化。UAV clear と upload の取り違えで初回 pop がゴミを返す。初期化前に town/reconcile が走らない順序保証を明記。
- **単一カウンタ atomic 競合（perf のみ）**: 32768 スレッドが単一 `DirtyCount`/`FreeCount` に InterlockedAdd → 直列化。正当性問題ではないがスループット注意。

---

## 是認できる部分（簡潔に）

- **絶対アドレッシングで大域揺れが消える論証（§5 インテリア全域）は正しい**。vp計算からカメラ中心が消える → 1フレーム遅れがインテリアで無害、という核は妥当。真因（index の中心相対性×1フレーム遅れ）の特定も筋が通る。
- **`desiredAPX = originX + ((sx-originX)&63)` は正しい**（窓内で apX≡sx(mod64) の一意値を返す）。`apX & 63` の負値2の補数 residue も正しい。剰余系の1:1（エイリアスなし）論証も窓内に限れば正しい（MEDIUM-5 の窓外リークを塞げば成立）。
- **free-list は 2パス分離規律下で race-safe**（上記解析の通り、二重確保・負数残留なし）。
- **枯渇時 PageTable=INVALID のグレースフル劣化**の方向性は妥当（ただし HIGH-3 の新規餓死は別途要対処）。
- **Stage 0（描画時CBでサンプル）を真因確定の使い捨て実験にする段階化**は健全。ビセクト用ゲート分離（ABS/CACHE/DIRTYDRAW）も良い。

**最優先で潰すべき**は CRITICAL-1（太陽変化 staleness）と CRITICAL-2（Requested クリア漏れ）。この2つはどちらも「テストは短時間で通るが実運用で確実に破綻する」型で、KEEP の正当性条件を residentAP 一致だけに置いた設計の穴を突く。

===== review:state-integ =====
コードベースの実装（`Vsm.hlsli` の center-relative `Vsm_VirtualPage`、`VsmAllocate_CS`、`VsmCasterDepth_VS`、`VsmSystem.cpp` の `NeedsRender`=matrix memcmp・resting/working 遷移）と設計を突き合わせて検証した。以下、state-integ 観点の辛口レビュー。

---

# VSM V5b 敵対的レビュー — state-integ

## 総評
真因分析（index の中心相対性 × 1フレーム遅れ）は**正しい**。実装の `Vsm_VirtualPage` は確かに `(lightXY - center)/extent` で中心相対、`UpdateConstants` の `cx=floor(camLX/pw)*pw` でテクセルは world-locked。絶対アドレッシングの方向性・トロイダル 1:1 論証・2の補数 mod・free-list 2パスは筋が良い。**ただし §5 の「揺れ消滅の証明」には致命的な穴があり、いくつかの状態ライフタイムが未定義で破綻する。**

---

## 【最重要】町サンプル側に residentAP 検証が無い → §5 の証明は index の一意性しか示していない

§5 の証明が示すのは「vp index がカメラ非依存で一意」まで。**「町が読む phys の中身が、その点の絶対ページと一致する」ことは証明していない。** 町は `PageTable[vp]→phys` を無検証で信頼するが、その map は**前フレームの origin で reconcile された**ものだ。

### 破綻シナリオ（先頭エッジ・64ページ変位）
- レベル L, `pw_L`。フレーム N で `originX` が +1（ページ境界跨ぎ）。
- 巻き込まれた新列の絶対ページ `apX = origin_N + 63`（= `origin_{N-1}+64`）。slot `sx=(origin_{N-1})&63`。
- そのスロットはフレーム N-1 reconcile 時点で `residentAP = origin_{N-1}`（＝反対側の旧ページ）を保持。
- 町フレーム N はこのスロットの phys をサンプル → **光空間で 64 ページ(=extent 分)離れた別ワールドの深度**を引く。

設計はこれを「1フレームだけ古い/未常駐」と書くが誤り。**未常駐(=光)ではなく、有効 phys が入った完全な別ワールドページ = 変位した偽シャドウ**。レベル0で `pw=4/16=0.25m`（baseExtent=4 と仮定）なら窓半幅32ページ ≈ 8m 先、境界跨ぎ毎に 0.25m 幅の偽シャドウ帯が近距離で 1 フレーム点滅する。これは「フリンジの無害な遅延」ではなく、**まさに残存しうる揺れ/ちらつき**。

### なぜガードバンド(§1.4)で消えないか
`d=2·maxΔ·64/62` → 「レベル L 使用条件 `maxΔ ≤ 31·pw_L`」。よってレベル L を引く点は `apX ∈ [origin+1, origin+63]`。**巻き込まれた列 `origin+63`（maxΔ=31, 境界）は含まれたまま**。1ページガードは OOB saturate を防ぐだけで、クロスフレーム・フリンジは消せない。2ページに広げても粗レベル L+1 が自前のフリンジを持つので原理的に不可能。

### 修正案（必須）
町 PS で**サンプル点の絶対ページと常駐ページを照合**する:
- `SlotDesc[vp].residentAP`（または PageTable entry に apX を同梱）を町から読めるようにし、`residentAP != floor(lightXY/pw)` なら **そのレベルを不採用**。
- 不一致時は**次の粗レベルへフォールバック**（粗レベルは窓スクロール頻度が半分で、跨ぎ直後でも一致確率が高い）。Unreal VSM と同じ多レベルフォールバック。最終的に全滅なら光。
- これで先頭エッジは「偽シャドウ」でなく「粗いが正しい影 or 光」になり、揺れが構造的に消える。

**この検証が無い限り §5 の「揺れ消滅」は成立しない。** インテリア（両フレーム窓内）が正しいのは是認するが、証明はインテリアしかカバーしていない。

---

## 【重要】req==0 即解放 → キャッシュが遮蔽変化で崩壊。「静止ほぼ0/移動数ms」は過大宣伝

§2.3 Pass1 は `req==0` で即 `FreePush`。ところが `Requested` は深度依存（`VsmPageMark_CS`）で、**同一窓内でも遮蔽・視錐台で可視集合が毎フレーム変わる**。

### 破綻シナリオ（可視チャーン）
- カメラが建物の陰へ→窓内だが遮蔽されたページ群が `req==0` → 解放。
- 次フレーム再可視 → `NeedAlloc` → 新規 phys → `MarkDirty` → 全再描画 + 上記フリンジ級の 1 フレーム遅延（light leak or 偽影）。

設計の「単軸移動 ≒ levels×VPPR のダーティ」「サブページ移動は全 KEEP」は**可視チャーンを無視**。移動中は遮蔽出入りで dirty 数が跳ね、静止キャッシュは「連続可視ページ」にしか効かない。町の中を歩くと実 dirty はこの見積りを大きく超える。

### 修正案
- **req==0 で解放しない。**常駐は world-locked で有効なので保持。解放は **free-list 枯渇時の LRU/age 退去**のみ（`PhysAP` に lastUsedFrame を追加）。これが本来の VSM キャッシュ挙動で、198ms→数ms の前提を実際に成立させる。

---

## 【重要】§6 の緩和スキップ案は不正。厳密 memcmp ゲートのみ許容

§6「全レベルの窓原点が不変なら VSM 更新丸ごとスキップ」は**破綻する**。原点不変でも可視集合（Requested）はサブページ移動・回転で変わる。スキップすると:
- 上記で解放されたページの再可視 → reconcile 走らず → `PageTable[vp]=INVALID` のまま → **影が抜ける**。

「原点不変 → 全 KEEP」は「全 in-window ページが常時常駐」を暗黙前提としており、可視チャーンで偽。

### 是認 & 修正
- 現行 `m_needsRender = memcmp(lightView) || memcmp(invViewProj)`（実装 128-129 行）**厳密ゲートはダーティ描画と正しく合成する**（needsRender=false なら RenderPages ごと不実行、DirtyCount 陳腐化も無害）。ここは是認。
- **緩和案は採用しない**、もしくは req==0 非解放（上記修正）とセットにしてのみ検討。req 非解放にすれば「原点不変 → 実質全 KEEP」が初めて真になり緩和が安全になる。

---

## 【要修正】ダーティフラグのライフタイム未定義 → 累積 OOB

`DirtyCount[0]` を**毎アクティブフレーム 0 にリセットする明示ステップが設計に無い**。append-only なので:
- リセット漏れ → DirtyCount 単調増加 → クリアクワッド indirect の instance 数がバッファ超過 → **OOB tile 座標で他タイル破壊**。

### 修正
- Pass1 直前に `DirtyCount[0]=0`（小 CS か ClearUAV）を明示。ペア数カウンタ（binning）も同様。
- `MarkDirty` を**冪等化**: 現状 `InterlockedAdd(DirtyCount)` 無条件 + `if(i<PHYS_CAP)` 書込み。1 phys が二重 MarkDirty されると count と list が desync → indirect が幽霊 instance を描く。`PhysDirty[phys]` を `InterlockedExchange` で 0→1 遷移した時だけ append せよ（phys 所有は 1 vp のはずだが REUSE と将来のマルチマーク混入に対する保険）。

---

## partial clear と atlas resting=PIXEL の両立 → 是認（1点注意）

- resource-level バリア（PIXEL↔DEPTH_WRITE, 実装 `BeginRenderStates`/`EndRenderStates` 踏襲）は**タイル単位描画と両立**。バリアは内容をクリアしないので非ダーティタイルは保持される。V5a の `m_pageTableState` 追跡拡張も妥当。**ここは是認。**
- クリアクワッド(ALWAYS)→キャスタ(LESS_EQUAL) は同一 DSV への連続描画でラスタ順が保証されるのでバリア不要。是認。
- 注意（perf のみ）: `ClearDepthStencilView` を捨ててクワッドで書くと HiZ/depth メタデータ圧縮が効かずタイル描画が僅かに重くなる可能性。正しさには影響なし。

---

## binning ダーティゲートの正しさ → 条件付き是認

- 「dirty ページにのみ caster×page を emit」は、KEEP=非描画が「residentAP 不変で内容有効」を満たすので**静的キャスタ前提では正しい**。binning と reconcile が同一フレーム・同一 `origin_N`（LevelWindow CB）を共有するので dirty 集合と vp 空間が整合。reconcile→UAV barrier→binning の順序厳守が条件。**是認。**
- **破綻シナリオ（動的キャスタ）**: 移動オブジェクトが KEEP ページ上を通過しても、そのページは dirty にならない（residentAP 不変・カメラ非スクロール）→ 影が凍結/残像。設計に**キャスタ移動によるダーティ化機構が皆無**。町が完全静的なら問題ないが、これは**明示的前提として書くべき**。動的要素を入れる瞬間、当該キャスタの「今フレーム+前フレーム」被覆ページを dirty にする追加パスが必要。

---

## その他の state-integ ハザード

1. **ランタイムゲート切替（DX12_VSM_CACHE 等）の状態不整合**: 永続バッファ(SlotDesc/PhysOwner/Atlas)は起動時 1 回 init。CACHE を実行中に off→on すると、非キャッシュ経路がアトラス内容を上書きした後 SlotDesc は古い phys→ページ対応を保持 → 再有効化直後に偽影。**ゲート切替時は永続バッファ全 reinit + アトラス invalidate（全 dirty 強制）を必須化**せよ。ビセクトツール自体が状態バグ源になる。

2. **1フレーム遅れの MarkPages 可視性**: `Requested` は「VSM post」なのでフレーム N 深度由来、町 N+1 が読む PageTable は N 可視性ベース。ディスオクルージョンした新可視ページは 1 フレーム INVALID→光漏れ。フリンジと同族の避けがたい遅延。residentAP フォールバック（最重要修正）が入ればこれも粗レベルで救える。

3. **reconcile タイミングによる緩和の余地**: 現状「VSM post で origin_N 使用 → 町は次フレーム読む」ため、町が読む map は常に 1 origin 遅れ、先頭列は構造的に不一致。もし PageTable の *index* だけでもフレーム N 頭（camera 既知）で origin_N reconcile し町 N が読めれば、新列は「別ページ phys」ではなく INVALID を返せ、**偽影→光漏れ**に格下げできる（光漏れの方が遥かに軽い）。ただし本命はやはり町側 residentAP 検証。

---

## 破綻シナリオまとめ（入力→誤結果）

| # | 入力/状態 | 誤結果 | 修正 |
|---|---|---|---|
| 1 | WSAD 前進でページ境界跨ぎ | 先頭列に 64ページ変位の偽影が 1F 点滅（残存揺れ） | 町 PS で residentAP 照合→粗レベルフォールバック |
| 2 | 建物陰への出入り（可視チャーン） | 再可視ページ全再描画＋1F 遅延、dirty 見積り破綻 | req==0 で解放しない、LRU 退去のみ |
| 3 | §6 緩和スキップ + サブページ移動 | 再可視ページの影抜け | 緩和不採用 or req 非解放とセット化 |
| 4 | DirtyCount リセット漏れ | 累積 instance で OOB→他タイル破壊 | 毎フレーム DirtyCount=0 明示 + MarkDirty 冪等化 |
| 5 | 動的キャスタが KEEP ページ通過 | 影凍結/残像 | 静的前提を明記、動的なら caster-motion dirty パス |
| 6 | CACHE ゲート実行中 on 切替 | 古い phys 対応で偽影 | 切替時 永続バッファ reinit + atlas 全 dirty |

## 是認する点
- 真因分析（中心相対 index × 1F 遅れ）と現行コードとの一致。
- トロイダル 1:1（完全剰余系）論証、`apX & 63` の 2の補数負値対応。
- インテリア（両フレーム窓内）の正しさ証明。
- free-list 2パス + UAVバリアによる push/pop 競合回避。
- resting/working 二態と partial 描画の両立、count-buffer による 0-draw 静止。
- Stage 0（代替A）を真因確定の使い捨て実験に使う手順。

**結論**: 絶対アドレッシングは方向として正しいが、**「揺れ消滅」は町側 residentAP 検証＋粗レベルフォールバックが無いと成立しない**（§5 は index 一意性しか証明していない）。加えて **req==0 即解放がキャッシュ性能主張を崩し**、**DirtyCount ライフタイムと緩和スキップが未定義/不正**。この3点（residentAP 検証・非解放LRU・dirty リセット&冪等）を Stage 2/3 の必須要件に格上げすべき。
