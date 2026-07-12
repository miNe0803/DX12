# VSM V5b 設計書 — トロイダル絶対アドレッシング + 安定割当 + ダーティ描画

以下、そのまま実装できる粒度でまとめる。まず**揺れの真因の確定**、次に**推奨方針と単純代替の比較**、そして各要件(1)〜(7)を数式付きで示す。

---

## 0. 揺れの真因（設計の出発点）

提供レポートで既に証明されている通り、**現行のアトラス・テクセルは world-locked**（`ox = cx - extent/2` が常に `pageWorld` の整数倍 → 物理テクセルの張り付き位置はワールド格子に固定）。したがって WSAD 揺れは「テクセル格子が滑る」からではない。

真因は **ページテーブル index (px,py) がカメラ中心相対**であること：
```
px_N(world) = floor(world/pw) - floor(camLX_N/pw) + 32   // 中心 C_N に依存
```
- アトラス／PageTable は前フレーム末（中心 `C_{N-1}`）に構築される（VSM はポスト、町はメイン）。
- 町パス（フレーム N, 中心 `C_N`）は `vp_N = f(C_N)` を計算して `PageTable[vp_N]` を読むが、常駐マップは `vp_{N-1} = f(C_{N-1})` で書かれている。
- `C_N ≠ C_{N-1}`（＝移動中）なら **全サンプルが 1 スロットぶんずれた entry を読む** → 大域的な揺れ。回転で揺れないのは中心スナップ量が回転で不変だから。移動でのみ `floor(camLX/pw)` が動く。

これが第一容疑(a)の厳密な機序。容疑(b)の「毎フレーム全再描画」は揺れの原因ではなく**速度**の問題（198ms）。

→ **揺れ = index の中心相対性 × 1フレーム遅れ**。速度 = 非キャッシュ全再描画。V5b は前者を絶対アドレッシングで、後者を安定割当＋ダーティ描画で潰す。

---

## 0.5 推奨方針 vs 単純代替（必読）

### 代替案 A：「相対のまま、アトラスを描いた時の CB で町がサンプルする」
CB を2重化し、**現在常駐しているアトラスを構築した時の CB（＝ `C_{N-1}`）**を町サンプルパスにバインドする。町は `vp_{N-1}` を計算 → 常駐 PageTable と一致 → **揺れは消える**。

- 長所: 変更が極小（CBダブルバッファ＋町のバインド差し替えのみ、半日）。アドレッシング総書き換え不要。
- 短所: **198ms は一切改善しない**（キャッシュ不可能。中心相対では「常駐ページが次フレームも有効」を判定できない）。CB とアトラス世代の同期がフラジャイル（世代ずれで即バグ）。将来のキャッシュ化への発展性ゼロ。

### 推奨：V5b（絶対アドレッシング）
揺れを消し、**かつ**キャッシュ（ダーティ描画）を可能にする。絶対アドレッシングでは常駐 phys が「絶対ワールドページ (apX,apY)」に紐づくので、窓内に残る限り再描画不要 → 198ms 解消の前提が成立する。代替案Aは揺れしか直せないので、198ms も要件に入っている以上**必ず絶対アドレッシングが要る**。V5b は代替Aを包含する。

**推奨手順**: まず Stage 0 として代替Aを 30分〜半日で入れ、「揺れが消える」ことを実機確認して真因(a)を確定させてから、V5b 本体（Stage 1〜3）に進む。代替Aは真因確定の使い捨て実験として価値がある。

---

## 1. トロイダル絶対アドレッシング

### 1.1 絶対ページ格子
レベル L の `pw_L = baseExtent·2^L / vppr = 4·2^L/64 = 2^L/16`。ライト空間点 `(lx,ly)` の**絶対ページ index**：
```hlsl
int apX = (int)floor(lx / pw);   // カメラ非依存、負値・大値OK
int apY = (int)floor(ly / pw);
```
ページ (apX,apY) は矩形 `[apX·pw, (apX+1)·pw) × [apY·pw, (apY+1)·pw)` を覆う。**構造的に world-locked**。

### 1.2 トロイダルスロット（2の補数マスク）
`vppr = 64 = 2^6` なので：
```hlsl
uint sx = (uint)(apX & (VPPR - 1));   // apX mod 64、負値も正residue（2の補数）
uint sy = (uint)(apY & (VPPR - 1));
uint vp = level * (VPPR*VPPR) + sy * VPPR + sx;   // 既存レイアウトと同一（32768）
```
`apX = -1` → `0xFFFFFFFF & 63 = 63`（＝数学的 mod と一致）。HLSL の `int &` は 32bit 2の補数ビット演算なので保証される。

### 1.3 アクティブ窓と 1:1（エイリアスなし）論証
フレーム N のカメラ光空間 `(camLX,camLY)`。レベル L の**窓原点**（CPU 算出、後述で CB へ）：
```
originX(L) = floor(camLX/pw_L) - VPPR/2     // ページ単位の整数
originY(L) = floor(camLY/pw_L) - VPPR/2
```
窓は絶対ページ `apX ∈ [originX, originX+VPPR)` を覆う。

**論証**: 窓幅 = VPPR = テーブル幅。任意の連続 VPPR 個の整数は mod VPPR の**完全剰余系**をなす。よって `sx = apX mod VPPR` は窓内で全単射 → 相異なる窓内ページは相異なるスロット → **エイリアスなし**。∎

窓のスクロール: `camLX` が `pw_L` 境界を跨ぎ `originX→originX+1` になると、剰余系のうち**ちょうど1列**（旧 `apX=originX`, sx=`originX&63`）が窓を出て、`apX=originX+VPPR`（同じ sx）が反対側から入る。→ その sx のスロット列（sy 全 64）だけが別ワールドページに切り替わる = ダーティ。他は不変。これがトロイダル・スクロールの本質でありキャッシュの源泉。

### 1.4 SelectLevel（変更ほぼ不要 + ガードバンド）
現行 `d = 2·max(|Δx|,|Δy|)`, `L = ceil(log2(max(d/base,1)))` は「maxΔ ≤ extent/2 = 32·pw」を保証 → 窓半幅にちょうど収まる。**境界ちょうど（maxΔ = 32·pw で apX = origin+VPPR）で窓外**になる縁ケースを避けるため、**1ページのガードバンド**を入れる：
```hlsl
// base を実効的に (VPPR/(VPPR-2)) 倍 ≒ わずかに大きく取り、maxΔ < (32-1)·pw を強制
d = 2.0 * maxAxis * (float)VPPR / (float)(VPPR - 2);
```
カメラ光空間 XY は `ZParams.zw`（非スナップ生値）を使う（現行踏襲）。SelectLevel は依然カメラ相対距離ベースで、絶対窓と両立する。

### 1.5 3ステージの統一数式
MarkPages / CasterDepth VS / TownPS の全てが `apX=floor(lightXY/pw)`, `sx=apX&63`, `vp=...` を計算する（カメラ中心を含まない）。inPageUV も world-locked：
```hlsl
float2 inPageUV = float2(lx/pw - floor(lx/pw), ly/pw - floor(ly/pw));   // = frac、ただしfloorと同一式で
```
`Vsm_PhysicalUV(phys,inPageUV,appr)` は現行のまま。**vp 計算にカメラ中心が入らない → 1フレーム遅れが無害**（§5 で証明）。

### 1.6 CB の変更
`Vsm_LevelCenterExtent[8]`（cx,cy,extent,texelWorld）を廃し、CPU で確定した**整数窓原点**を渡してステージ間の float-floor 差異を排除：
```cpp
struct LevelWindow { float pageWorld; float texelWorld; int originX; int originY; }; // 16B
// CPU:
float pw = kBaseExtent * (1u<<L) / kVirtualPagesPerRow;
int   ox = (int)floorf(camLX / pw) - kVirtualPagesPerRow/2;   // = originX(L)
int   oy = (int)floorf(camLY / pw) - kVirtualPagesPerRow/2;
Vsm_LevelWindow[L] = { pw, pw/kPageSize, ox, oy };
```
`apX = floor(lightX/pw)` 自体は各ワールド点で決定的（同じ lightX と pw なら全シェーダ一致）なので、`originX/Y` は reconcile の「desired AP」算出と SelectLevel 窓判定にのみ使う。

---

## 2. 安定割当 + ダーティ検出

### 2.1 永続バッファ（フレーム跨ぎで**クリアしない**）
| バッファ | 要素 | 型 | 用途 |
|---|---|---|---|
| `SlotDesc[32768]` | vp毎 | `int2 residentAP; uint phys; uint valid` (16B) | 各スロットが現在保持する絶対ページ・物理 |
| `PageTable[32768]` | vp毎 | `uint` | 町サンプル用 phys（`0xFFFF`=未常駐）。reconcile が導出 |
| `PhysOwner[4096]` | phys毎 | `uint` | どの vp が所有するか（一貫性検証・再利用） |
| `PhysAP[4096]` | phys毎 | `int2 ap; uint level` (12B, 16Bパディング) | BuildPageParams のワールド矩形算出 |
| `FreeList[4096]` + `FreeCount[1]` | — | `uint`スタック | GPU free-list |
| `DirtyList[4096]` + `DirtyCount[1]` | — | `uint` append | 当フレームのダーティ phys |

初期化（起動時 CS もしくは CPU upload、1回）: `FreeList=[0..4095]`, `FreeCount=4096`, 全 `SlotDesc.valid=0`, `PhysOwner=INVALID`。

### 2.2 desired AP（スロットが**今**保持すべき絶対ページ）
MarkPages は現行同様 `InterlockedOr(Requested[vp],1)` のみ（apX を書かなくてよい — 窓内 1:1 なので一意に導出可能）：
```hlsl
int desiredAPX(uint sx, int originX) { return originX + (int)((uint)((int)sx - originX) & (VPPR-1)); }
// desired = [originX, originX+VPPR) の中で apX≡sx(mod VPPR) となる唯一値
```
これは可視ピクセルが持つ apX と一致（可視点は §1.4 で窓内保証）。

### 2.3 reconcile（2 パス — free-list の push/pop 競合回避）

**Pass 1: Release / Keep / Reuse**（1スレッド=1 vp, 32768）
```hlsl
uint vp = id.x; if (vp >= TOTAL_VP) return;
uint level = vp/(VPPR*VPPR), rem = vp%(VPPR*VPPR), sy=rem/VPPR, sx=rem%VPPR;
LevelWindow w = Vsm_LevelWindow[level];
int2 desired = int2(desiredAPX(sx, w.originX), desiredAPY(sy, w.originY));

VsmSlot s = SlotDesc[vp];
uint req = Requested[vp];              // MarkPages 由来
NeedAlloc[vp] = 0;

if (req == 0) {                        // 非可視 → 解放
    if (s.valid && s.phys != INVALID) { FreePush(s.phys); PhysOwner[s.phys]=INVALID; }
    s.valid = 0; s.phys = INVALID; PageTable[vp] = INVALID;
}
else if (s.valid && s.phys!=INVALID && s.residentAP.x==desired.x && s.residentAP.y==desired.y) {
    PageTable[vp] = s.phys;            // KEEP: 内容は world-locked で有効 → 非ダーティ、再描画しない
}
else if (s.valid && s.phys!=INVALID) { // REUSE: スクロールで AP が変わった列。同じ phys を転用
    s.residentAP = desired; PhysOwner[s.phys]=vp; PhysAP[s.phys]=int3(desired,level);
    PageTable[vp] = s.phys;  MarkDirty(s.phys);       // DirtyList へ append
}
else { NeedAlloc[vp] = 1; PageTable[vp] = INVALID; }  // 新規 → Pass2 で確保
SlotDesc[vp] = s;
```
UAV バリア（`FreeCount`/`FreeList`/`DirtyList` 確定）を挟む。

**Pass 2: Alloc from free-list**
```hlsl
uint vp = id.x; if (vp >= TOTAL_VP || NeedAlloc[vp]==0) return;
uint phys = FreePop();
if (phys != INVALID) {
    ... desired/level 再計算 ...
    VsmSlot s; s.residentAP=desired; s.phys=phys; s.valid=1; SlotDesc[vp]=s;
    PhysOwner[phys]=vp; PhysAP[phys]=int3(desired,level);
    PageTable[vp]=phys; MarkDirty(phys);              // 新規は必ずダーティ
} else {
    PageTable[vp]=INVALID;                            // プール枯渇 → §2.5
}
```

### 2.4 GPU free-list（atomic）
```hlsl
void FreePush(uint phys){ uint i; InterlockedAdd(FreeCount[0], 1u, i); FreeList[i]=phys; }
uint FreePop(){
    uint old; InterlockedAdd(FreeCount[0], (uint)(-1), old);   // old = 減算前
    if ((int)old > 0) return FreeList[old-1];
    InterlockedAdd(FreeCount[0], 1u, old);                     // アンダーフロー復旧
    return INVALID;
}
void MarkDirty(uint phys){ uint i; InterlockedAdd(DirtyCount[0],1u,i); if(i<PHYS_CAP) DirtyList[i]=phys; }
```
Pass1 で全 push が完了 → Pass2 で pop するので、当フレーム解放分も再利用可能。KEEP/REUSE は free-list に触れない（スクロール列は REUSE で phys を保持したまま転用 → free-list トラフィック最小）。

### 2.5 物理 4096 超過
`FreePop()==INVALID` → `PageTable[vp]=INVALID` → 町は「未常駐」＝影なし（現行 `phys<gPhysCap` と同じグレースフル劣化、退行なし）。任意の改善: 枯渇時に「可視だが最粗レベルの KEEP ページ」を犠牲にする優先度制御を追加可能だが V5b では非推奨（後回し）。実運用の可視窓（各レベル最大 VPPR² だが実際は視錐台で疎）で 4096 を超えるのは病的視点のみ。`FreeCount==0` を HUD 監視。

---

## 3. ダーティのみ描画

### 3.1 ビニング
現行 BuildCasterBinning の「割当済みか」判定を「ダーティか」に変える。最小改修: count/scatter でページ候補の phys を求め `slot.flags & DIRTY`（または `PhysDirty[phys]`）でのみペア emit。あるいは `DirtyList[0..DirtyCount)` を反復してキャスタ×ダーティページのみ交差判定。ペア総数が激減 → m2b〜m2e が高速化。

### 3.2 RenderPages（アトラス全クリア廃止 → タイル単位、GPU駆動）
`ClearDepthStencilView(NumRects=0)` を**やめる**。アトラスは永続リソースなので**非ダーティタイルの内容は保持される**。ダーティタイルのみを以下で更新：

1. **クリアクワッド パス**（indirect, `DirtyList` を instance に）: ダーティ phys ごとにアトラスタイル `(tx·128, ty·128, 128×128)` を覆う三角形を描き `SV_Depth=1.0` を書く。DepthFunc = `ALWAYS`。
2. **キャスタ パス**（既存 ExecuteIndirect, ダーティペアのみ）: DepthFunc = `LESS_EQUAL`。VS が §4 の絶対配置でタイル内へ描く。

両パスとも `ExecuteIndirect` の **count buffer に `DirtyCount`**（クリア）/ ペア数を使い、`DirtyCount==0`（静止・サブページ移動）なら **0 draw = 実質ゼロコスト**。CPU readback 不要。

NumRects 付き `ClearDepthStencilView` 版は dirtyCount が GPU 側で CPU readback（1フレーム遅れ or ストール）を要するため**非推奨**。クリアクワッド方式を採る。

---

## 4. BuildPageParams（絶対）
ダーティ phys（または全常駐）について、`PhysAP[phys] = (apX,apY,level)` から：
```hlsl
float pw = Vsm_LevelWindow[level].pageWorld;
float ox = (float)apX * pw;          // 絶対ライト空間原点（world-locked）
float oy = (float)apY * pw;
PageCenterExtent[phys] = float4(ox, oy, pw, (float)level);   // 中心+extent ではなく「原点+pageWorld」
PageTile[phys]         = uint4(sx, sy, phys % appr, phys / appr);
```
reconcile が既に apX/level を持つので、`PageCenterExtent`/`PageTile` を **reconcile 内で直接書けば BuildPageParams を省略可能**（推奨最適化）。明快さのため段階実装では別 CS のままでよい。

**CasterDepth VS（絶対）**:
```hlsl
float3 ls = mul(float4(worldPos,1), Vsm_LightView).xyz;
float lx = (ls.x - ox) / pw;         // inPageUV.x ∈ [0,1]
float ly = (ls.y - oy) / pw;
float ax = ((float)tx + lx) / appr;  // アトラス UV
float ay = ((float)ty + ly) / appr;
Out.pos = float4(ax*2-1, 1-ay*2, Vsm_NormalizeDepth(ls.z, zNear, zFar), 1);  // Y反転は現行踏襲
```
カメラ中心が一切現れない。

---

## 5. 1フレーム遅れの整合（揺れ消滅の証明）

絶対アドレッシングでは：
- phys P の**テクセル内容**は絶対ワールドページ (apX,apY,L) の深度（world-locked）。
- 町（フレーム N）はワールド点から `vp = L·..+(apY&63)·64+(apX&63)` を計算 — **カメラ中心非依存**。`PageTable[vp]`（フレーム N-1 末に構築）を読む。

場合分け：
- **その点の (apX,apY) が N-1 の窓内だった**（＝インテリア、視界のほぼ全域）: `SlotDesc[vp].residentAP == (apX,apY)`、phys は正しいワールドページの world-locked 深度 → **完全に正しい**。カメラがどれだけ移動していても揺れない。
- **その点が今フレーム新規に窓へ入った縁（フリンジ）**: N-1 はそのスロットに別の絶対ページ（巻き戻った側）を格納 → 1フレームだけ古い/未常駐 → 次フレーム reconcile で更新。**フリンジの1フレーム遅延のみ**で、大域揺れではない。

現行（中心相対）との対比: 中心相対では vp index 自体が中心で動くため、**中心が変わった瞬間に全サンプルが誤entryを読む → 大域揺れ**。絶対では**インテリア全域が常に正しい** → 揺れ消滅。∎

代替案A（中心相対＋描画時CBでサンプル）も、町が「常駐アトラスを構築した中心 `C_{N-1}`」で vp を計算すれば常駐 PageTable と一致 → 揺れは消える。ただしキャッシュ不可・世代同期フラジャイル（§0.5）。**推奨は絶対**。

---

## 6. クロスフレーム状態・静止スキップとの整合

- **Atlas**: resting=`PIXEL_SHADER_RESOURCE`(町サンプル), working=`DEPTH_WRITE`(RenderPages)。V5a の Begin/EndRenderStates を踏襲。**全クリアを廃した結果、内容がフレーム跨ぎで永続** — これがキャッシュの前提。
- **PageTable**: resting=`PIXEL_SHADER_RESOURCE`(町), working=`UAV`(reconcile)。V5a の `m_pageTableState` 追跡を踏襲。V5b では**永続化**するが、reconcile が毎アクティブフレーム全 vp を書くので古い entry は残らない。
- **新規永続バッファ**(SlotDesc/PhysOwner/PhysAP/FreeList/FreeCount/DirtyList/DirtyCount): 町からは触らないので **UAV のまま据え置き**。reconcile 2パス間・binning 前後に UAV バリアのみ。resting/working 二態は不要。
- **静止スキップ (V5a NeedsRender)**: `lightView`/`invViewProj` の memcmp を維持。絶対アドレッシングでは常駐が world-locked & 正しく index されるため**より安全**。緩和案: 「全レベルの窓原点が不変 かつ 太陽/視錐台不変」なら VSM 更新丸ごとスキップ。サブページ移動（原点不変）は reconcile 走っても全 KEEP → `DirtyCount==0` → クリア/キャスタ indirect が 0 draw = 実質ゼロ。`DirtyCount` count buffer で自然にゼロコスト化。

**内容漏れの不在**: 解放された phys は `PageTable`/`PhysOwner` から即外れ、再確保時に必ず `MarkDirty` → 同フレームで再描画されてから翌フレーム町がサンプル。有効スロットが古い内容を指す瞬間は存在しない（唯一の遅延はフリンジ＝別ワールドページ、内容誤りではない）。

---

## 7. 段階実装順と検証

### Stage 0（真因確定・使い捨て, 半日）
CB を2重化し、**常駐アトラスを描いた CB(`C_{N-1}`)** を町サンプルにバインド。→ WSAD 揺れが消えれば真因(a)確定。198ms は不変。以後 V5b へ。

### Stage 1（絶対アドレッシング統一・キャッシュなし）
`Vsm_VirtualPage` と全呼出（MarkPages / CasterDepth VS / TownPS / BuildPageParams / binning）を絶対式へ。**割当は非キャッシュ全再割当・全クリア・全再描画のまま**（198ms 維持）。CB を `LevelWindow` へ。
- 検証: 影正しい / **WSAD 揺れ消滅** / 回転OK。readback で既知ワールド点の vp がカメラ移動で**不変**を確認。アトラス debug: カメラ平行移動でテクスチャがページ単位でスクロールするのみ（滑らない）。
- リスク: `apX & 63` の負値正当性（カメラを負のライト空間へ）。SelectLevel 縁ガードバンド（§1.4）。

### Stage 2（安定割当＋ダーティ検出・描画はまだ全クリア）
SlotDesc/PhysOwner/PhysAP/FreeList/PhysAP + 初期化CS 追加。VsmAllocate を reconcile 2パスへ置換。
- 先に **DIRTY を強制的に全 ON**（KEEP無効化）→ Stage 1 と同一出力（退行チェック）。
- 次に KEEP 有効化: 静止=`DirtyCount 0`、単軸移動=`≒ levels×VPPR`、対角=やや多、を readback で確認。
- free-list 整合: `FreeCount ∈ [0,4096]`、`PhysOwner` と `SlotDesc` の相互一貫性を走査する検証CS（二重所有=0 を assert）。
- リスク: push/pop 競合（2パス＋UAVバリア必須）。REUSE 時の AP 完全一致でのみ KEEP。

### Stage 3（ダーティのみ描画）
reconcile で `DirtyList` 構築。binning をダーティページのみに。RenderPages: 全クリア撤去 → クリアクワッド(indirect, DepthFunc ALWAYS) + キャスタ(indirect, LESS_EQUAL)、両者 `DirtyCount`/ペア数 count buffer 駆動。
- 検証: フレーム時間 198ms → 移動中数ms・静止ほぼ0。非ダーティ領域に穴なし（内容保持）／再利用ページに古い影残らない（ダーティ再描画で被覆）。高速WSAD＋回転でフリンジ1フレーム遅延のみ。`FreeCount==0`(CAP HIT) を HUD 監視。
- リスク: クリアクワッドとキャスタの DepthFunc 併用（クリア=ALWAYS 先, キャスタ=LESS_EQUAL）。indirect count buffer 配線（CPU readback ストール回避）。

### 追加検証ツール
- `DirtyCount`/`FreeCount` を毎フレーム readback → HUD。
- 既存 `RenderAtlasDebug` で永続性＋タイルクリア確認。
- `SlotDesc↔PhysOwner` 一貫性検証CS（assert カウンタ）。
- ランタイムゲート `DX12_VSM_ABS` / `DX12_VSM_CACHE` / `DX12_VSM_DIRTYDRAW` を分離しビセクト可能に（`DX12_VSM` の下位フラグ）。

---

## 主要変更ファイル
- `Shaders/Vsm/Vsm.hlsli` — `Vsm_VirtualPage` を絶対式へ（§1.2）、`Vsm_SelectLevel` にガードバンド（§1.4）。
- `Shaders/Vsm/VsmPageMark_CS.hlsl` — 絶対 vp（apX計算不要, Requested のみ）。
- `Shaders/Vsm/VsmAllocate_CS.hlsl` → **reconcile 2 CS へ置換**（§2.3）+ free-list（§2.4）。
- `Shaders/Vsm/VsmBuildPageParams_CS.hlsl` — 絶対原点＋pageWorld（§4）、または reconcile へ統合。
- `Shaders/Vsm/VsmCasterDepth_VS.hlsl` / `VsmBinning.hlsli` — 絶対配置＋ダーティのみ（§3.1,§4）。
- `Shaders/Town/TownPS.hlsl` — 絶対サンプル（§1.5）。
- `Source/Graphics/VsmSystem.h/.cpp` — 永続バッファ・ヒープ・初期化CS・クリアクワッド/count buffer・`LevelWindow` CB・reconcile ディスパッチ・状態追跡拡張（§2.1,§3.2,§6）。

**結論**: 揺れの真因は「ページテーブル index の中心相対性 × 1フレーム遅れ」。トロイダル絶対アドレッシング（V5b）が揺れとキャッシュ不能の両方を根治する唯一の方針で、単純代替（描画時CBでサンプル）は揺れのみ直し 198ms を残すため**推奨しない**（ただし Stage 0 の真因確定実験としては有用）。
