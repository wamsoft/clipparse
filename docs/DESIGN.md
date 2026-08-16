# clipparse 設計検討 — psdparse 型の遅延参照ライブラリは CLIP で成立するか

結論から: **成立する。しかも PSD よりきれいに成立する。**

理由は CLIP が「メタ情報 (SQLite) と実データ (バイナリ領域) が最初から分離されており、
かつ SQLite 側に実データの絶対オフセット表 (`ExternalChunk`) と
ブロック単位のサイズ表 (`Offscreen.Attribute.BlockSize[]`) の両方がある」から。
[根拠は CLIP_FORMAT.md §2.1 / §3.3 / §8、実ファイルで検証済み]

PSD では「レイヤレコードを頭から順に舐めて初めて各チャンネルの位置が分かる」ので
`IteratorBase` を持ち回る必要があったが、CLIP では **メタ情報を読んだ時点で
全ピクセルブロックの (offset, length) が確定する**。ランダムアクセスが素直に書ける。

---

## 1. psdparse の設計おさらい

| 目標 | 実現手段 |
|---|---|
| 遅延 I/O | `IteratorBase` (純粋仮想) 経由。mmap またはストリーム。パース時は構造メタのみ読む |
| バックエンド非依存 | `MemoryReader` (mmap/連続バッファ) と `StreamReader`+`Source` の 2 実装 |
| 範囲安全 | `cloneRange(offset, len)` で厳密に境界付けした子リーダを作る。`SubBlock` RAII |
| 往復保存 | 再直列化が面倒なブロックは生バイトの iterator を保持して save 時にそのまま流す |
| 依存最小 | C++17 + zlib のみ |

`psd::Data` のフィールドに `IteratorBase*` のクローンが刺さっており、
クローンが `MemoryReader` なら mmap 内へのポインタ、`StreamReader` なら
`shared_ptr<Source>` を持つ。最後のクローンが死ぬとバックエンドが閉じる。

---

## 2. CLIP へのマッピング

| psdparse | clipparse 相当 | 備考 |
|---|---|---|
| PSD ヘッダ | `CSFCHUNK` + `CHNKHead` | 24 + 40 バイト |
| レイヤレコード列 | SQLite `Layer` テーブル | ツリーはリンクリスト |
| チャンネルデータ | `CHNKExta` ペイロード内のブロック列 | 256x256 タイル + zlib |
| チャンネル位置の算出 | `ExternalChunk.Offset` + `BlockSize[]` 累積和 | **走査不要** |
| 画像リソース | SQLite の各テーブル | 関係データベースそのもの |
| 追加レイヤ情報 (lsct/luni/TySh…) | `Layer` の BLOB 列 (`TextLayerAttributes` 等) | |
| 生バイト保持による往復保存 | 「触っていない external チャンクは元ファイルからコピー」 | §6 |

### 2.1 遅延の粒度

PSD が「レイヤ 1 枚単位」だったのに対し、CLIP は **256x256 ブロック単位** で
遅延できる。4000x4000 のレイヤから 512x512 の一部だけ欲しい、という要求に対して
該当ブロック 4〜9 個だけ zlib 展開すればよい。これは psdparse では
(PackBits の行スキャンが必要なため) 難しかった芸当。

### 2.2 メモリ試算

`test000.clip` (1.1 MB) 実測:

```
ファイル全体            1,120,137 B
バイナリ領域              395,113 B   ← 遅延対象
SQLite 領域               724,992 B
  うち ParamScheme/ElemScheme が大半 (1179 + 61 行の UI スキーマ、描画に不要)
Offscreen.Attribute      198〜282 B × 24 行 = 約 5 KB   ← 常駐させたいメタ
```

常駐メタは **レイヤ 1 枚あたり約 1.5 KB** (6 Offscreen × ~250B)。
1000 レイヤでも 1.5 MB。psdparse の「数百 KB」と同オーダーで問題なし。

本番ファイル実測 (60〜91 MB):

| ファイル | 全体 | SQLite 領域 | 占有率 | レイヤ | Offscreen |
|---|---|---|---|---|---|
| tama.clip | 60 MB | 6.9 MB | 11.5% | 65 | 488 |
| nazoani01_ja.clip | 63 MB | 7.8 MB | 12.3% | 452 | 3144 |
| haruse-ja.clip | 91 MB | 5.1 MB | 5.6% | 206 | 1877 |

**遅延対象 (バイナリ領域) が 88〜94% を占める**ので、遅延の利得はそのまま出る。
SQLite 領域の 5〜8 MB は **mmap の上にゼロコピーで載せられる** (次節) ので
実メモリを食わない。`Attribute` を全部パースしても数百 KB。

---

## 3. SQLite をどう扱うか (最大の設計判断)

CLIP のメタは SQLite ファイルそのものなので、選択肢は 3 つ:

| 案 | 内容 | 評価 |
|---|---|---|
| A | 一時ファイルへ書き出して `sqlite3_open` | clip-tools 方式。実装は最短だが、数 MB の I/O とテンポラリ管理が発生。ストリーム入力でも一時ファイルが要る |
| B | `sqlite3_deserialize()` に mmap 領域を渡す | **推奨**。`SQLITE_DESERIALIZE_READONLY` かつ FREEONCLOSE なしなら SQLite は渡したバッファを読むだけ。コピーもテンポラリも不要 |
| C | SQLite フォーマットを自前パース | 依存は減るが B-tree/overflow page/varint を実装することになる。割に合わない |

**推奨は B**。ロード時に必要な作業は:

1. チャンクを歩いて `CHNKSQLi` の (offset, length) を得る
2. `sqlite3_open(":memory:")` → `sqlite3_deserialize(db, "main", mmap_base + offset, len, len, SQLITE_DESERIALIZE_READONLY)`
3. 以後は普通に `SELECT`

ストリーム入力 (mmap できない) のときだけ、SQLite 領域を `std::vector` に読み込んで
同じ API に渡す (領域長は分かっているので一発で確保できる)。この場合も
**バイナリ領域は遅延のまま**なので、大きいのは常に遅延側という性質は保たれる。

依存関係は「zlib + sqlite3 (amalgamation 1 ファイル)」に増える。psdparse の
「第三者フレームワークに依存しない」方針は維持できる。

### 3.1 「動的な構造を維持する」という要件について

ここが CLIP の一番おいしいところ。

PSD では全フィールドを C++ 構造体に写し取る必要があった (`psddata.h` が 626 行)。
CLIP は **メタ情報が最初から関係データベース**なので、

- 汎用アクセス: `doc.table("Layer").row(i).get("LayerOpacity")` のような
  動的ビューを 1 本用意すれば、**モデル化していない列にも即座に到達できる**
- 型付きアクセス: よく使う `Layer` / `Canvas` / `Offscreen` は
  C++ 構造体 (または属性アクセサ) を被せて型安全にする

の 2 段構えにできる。CSP のバージョン差で列が増減しても、汎用側は壊れない。
clip-tools が `LayerRecord.from_row` で「知らない列が値を持っていたら例外」に
しているのは、**動的アクセスの逃げ道が無い**からで、こちらはその制約を負わない。

**方針**: 動的ビューを基層に置き、型付きビューはその上の薄いラッパにする。

---

## 4. 提案するクラス構成

```
IteratorBase            (psdparse から流用可: バイト供給の抽象)
├── MemoryReader        mmap / 連続バッファ
└── StreamReader        任意の seekable ストリーム (+ Source)

ClipFile
├── mapping_            mmap またはバッファ
├── chunkIndex_         external_id → {offset, dataSize}   (CHNKExta を 1 度だけ走査)
├── db_                 sqlite3* (deserialize 済み、READONLY)
├── tables()            動的テーブルビュー
├── canvas()            型付き: 幅/高さ/解像度/ルートフォルダ
├── layers()            型付き: ツリー (LayerFirstChildIndex/LayerNextIndex を解決済み)
└── image API
    ├── layerImage(layerId, mode, scale=100)   → 全面 RGBA
    ├── layerRegion(layerId, rect)             → 部分だけブロック展開
    └── blockData(offscreenId, blockIndex)     → 生ブロック 1 枚 (最下層)

ClipLayer   (ツリーノード。属性は SQLite 行への参照で、コピーを持たない)
ClipOffscreen (Attribute をパースした {w,h,cols,rows,nchan,initColor,blockSizes})
```

**ミップ連鎖の解決は必ず `Layer.LayerRenderMipmap` → `Mipmap.BaseMipmapInfo` →
`MipmapInfo.NextIndex` の順で辿ること。** `MipmapInfo` を `(LayerId, ThisScale=100)`
で引くとマスク用連鎖と衝突する (CLIP_FORMAT.md §2.3)。`ClipLayer` は
描画用 / マスク用の 2 本を別々に持たせる。

### 4.1 ロード時にやること / やらないこと

**やる**:

- チャンク走査 (`CHNKExta` の外部 ID と offset を index 化)。ここだけは
  ファイル全体をシークする必要があるが、**読むのは各チャンクの 72 バイトヘッダのみ**。
  ペイロードは触らない (`seek` で飛ばす)。
  - 補足: `ExternalChunk` テーブルにも同じ offset があるので、
    SQLite を先に開けば **走査を完全に省略**できる。ただし SQLite 領域を先に
    見つけるにはチャンクを歩く必要がある…という循環がある。
    `CHNKHead.binary_section_size` が `CHNKSQLi` の位置そのものなので、
    **先頭 64 バイトだけ読めば SQLite に直行できる** [実測: 395113 で一致]。
    これが最速経路。整合性チェックとして走査版も残す。
- SQLite の deserialize
- `Layer` ツリーの構築 (数百行の SELECT)

**やらない**:

- `Offscreen.Attribute` のパース (アクセス時に遅延、結果はキャッシュ)
- ブロックの zlib 展開
- `ParamScheme` / `ElemScheme` の読み出し (触らなければコストゼロ)

### 4.2 ブロックアドレスの算出

```cpp
// SQLite だけで完結する。バイナリ領域は最後に 1 回だけ触る。
int64_t chunkOff  = externalChunkOffset(offscreen.blockDataId);
int64_t dataStart = chunkOff + 16 + 56;
int64_t rel = 0;
for (int i = 0; i < blockIndex; ++i) rel += attr.blockSizes[i];
// レコード先頭 74 バイトを読んで検証 (record_size / has_content / decompressed_size)
// zlib 本体は dataStart + rel + 74 から compressed_size バイト
```

`blockSizes` の前置和を一度作っておけば O(1)。

---

## 5. 共通 API (psdparse と揃えられる部分)

「PSD と CLIP を同じ顔で触る」ための最小共通面。psdparse の Python API に寄せる。

### 5.1 共通化できる

| 概念 | 共通 API | PSD | CLIP |
|---|---|---|---|
| 文書 | `width`, `height`, `resolution` | header | `Canvas` |
| レイヤ列 | `layers[]` (下から上) | `layerList` | `Layer` リンクリストを解決 |
| 階層 | `parent`, `children`, `is_group` | lsct 区切り | `LayerFolder` bit0 |
| 名前 | `name` (UTF-8) | `luni` | `LayerName` |
| 可視 | `visible` | flag bit1 の反転 | `LayerVisibility` |
| 不透明度 | `opacity` (0..255 に正規化) | 0..255 | `LayerOpacity` 0..**256** → `round(v*255/256)` |
| 合成モード | `blend_mode` (共通 enum) | 4 文字キー | `LayerComposite` |
| クリッピング | `clipping` | `clipping` | `LayerClip` |
| 矩形 | `bounds` = (left, top, width, height) | レイヤ矩形 | `LayerOffsetX/Y` + offscreen の w/h |
| 画素取得 | `layer_image(i, mode)` → RGBA/BGRA bytes | チャンネル合成 | ブロック合成 |
| 合成画像 | `merged_image()` | image data section | `CanvasPreview` / 自前合成 |
| マスク | `mask` (矩形 + 画素) | layer mask | `LayerLayerMaskMipmap` 経由の offscreen |
| テキスト | `text` (本文 + ラン) | EngineData | `TextLayerString` + `TextLayerAttributes` |

### 5.2 共通化できない (各形式固有)

- PSD 固有: 画像リソース、レイヤカンプ、Descriptor、レイヤ効果 (lfx2)、ブレンド範囲
- CLIP 固有: ミップマップ階層、ベクタレイヤ、ブラシ定義、3D/カメラ/タイムライン、
  定規、パススルー (`PASS_THROUGH` は PSD の `pass` に対応するが挙動差あり)

**設計方針**: 共通面は `imgdoc::Document` / `imgdoc::Layer` のような
純粋仮想インタフェース (またはヘッダオンリーのコンセプト) にし、
`psd::PSDFile` と `clip::ClipFile` がそれを実装する。
固有機能は各具象クラスの API として素で出す (共通面に押し込めない)。

### 5.3 合成モード対応表

| CLIP `LayerComposite` | PSD キー | 備考 |
|---|---|---|
| 0 NORMAL | `norm` | |
| 1 DARKEN | `dark` | |
| 2 MULTIPLY | `mul ` | |
| 3 COLOR_BURN | `idiv` | |
| 4 LINEAR_BURN | `lbrn` | |
| 5 SUBTRACT | `fsub` | |
| 6 DARKER_COLOR | `dkCl` | |
| 7 LIGHTEN | `lite` | |
| 8 SCREEN | `scrn` | |
| 9 COLOR_DODGE | `div ` | |
| 10 GLOW_DODGE | `div ` | **実測で標準 color dodge と一致** (CLIP_FORMAT.md §9) |
| 11 ADD | `lddg` | 未測定。10/12 の結果からすると、むしろこちらが CSP 固有の可能性 |
| 12 ADD_GLOW | `lddg` | **実測で標準 linear dodge と一致** |
| 13 LIGHTER_COLOR | `lgCl` | |
| 14 OVERLAY | `over` | |
| 15 SOFT_LIGHT | `sLit` | |
| 16 HARD_LIGHT | `hLit` | |
| 17 VIVID_LIGHT | `vLit` | |
| 18 LINEAR_LIGHT | `lLit` | |
| 19 PIN_LIGHT | `pLit` | |
| 20 HARD_MIX | `hMix` | |
| 21 DIFFERENCE | `diff` | |
| 22 EXCLUSION | `smud` | |
| 23 HUE | `hue ` | |
| 24 SATURATION | `sat ` | |
| 25 COLOR | `colr` | |
| 26 LUMINOSITY | `lum ` | |
| 30 PASS_THROUGH | `pass` | フォルダのみ |
| 36 DIVIDE | `fdiv` | |

27,28,29,31..35 は未使用/未確認。**8 種は `blendmodes.clip` で実測同定済み**
(1 / 2 / 8 / 10 / 12 / 14 / 21 / 26)。式は CLIP_FORMAT.md §9。
「発光」付きが標準式と一致したのは意外な結果で、PSD 変換で非可逆になるのは
むしろ発光なしの 9 / 11 の側だと予想される (要測定)。

---

## 6. 書き込み・往復保存の見通し

psdparse の「触っていない部分は生バイトのまま流す」戦略は CLIP でもそのまま効く。
むしろ CLIP のほうが分離が良いので楽:

```
save(path):
  1. CSFCHUNK ヘッダを書く (サイズは後でパッチバック)
  2. 各 external チャンクを書く
       - 触っていない → 元ファイルの [offset, offset+72+dataSize) をそのままコピー
       - 差し替えた   → ブロックを再エンコードして書き、新しい offset を控える
  3. SQLite を書く
       - 触っていない → 元領域をそのままコピー
       - 触った       → deserialize したものを書き戻し可能な DB へコピーして UPDATE
                        (最低限 ExternalChunk.Offset は必ず書き換わる)
  4. CHNKFoot、先頭のサイズをパッチバック
```

**難所**: `ExternalChunk.Offset` はバイナリ領域内の絶対オフセットなので、
チャンクを 1 つでも差し替えると以降全部の offset がずれ、SQLite の更新が必須になる。
つまり「メタは無変更で画素だけ差し替え」は原理的に不可能。
→ SQLite を書き換え可能にする経路 (READONLY を外す or 一度コピーする) が必ず要る。

段階案:

- **W0 (読み専用)**: save なし。まずここまで。
- **W1**: レイヤ属性 (名前/不透明度/可視/合成モード) の編集 → SQLite の UPDATE のみ。
  バイナリ領域は無変更なので offset も動かない。**もっとも安全で価値が高い**。
- **W2**: 画素差し替え → チャンク再配置 + `ExternalChunk` 更新 + `Attribute.BlockSize[]` 更新。
- **W3**: レイヤ追加/削除 → `Layer` のリンクリスト再構築 + `Offscreen`/`MipmapInfo` の生成。

### 6.1 テンプレート .clip を種にする方式 [実測に基づく]

「ゼロから SQLite を組む」のは非現実的なので、**CSP で新規作成 → 即保存したファイルを
テンプレートとして開き、そこに足していく**のが唯一現実的な経路。
`samples/emptyimage.clip` (1600x1200) / `samples/emptyanime.clip` (864x648) がその実物。

```
emptyimage.clip  267 KB … うち SQLite が 97% (ParamScheme 1250 行が大半)
                 Layer 3 行 (ルートフォルダ / 用紙 / レイヤー 1)
                 Offscreen 18 行 = 3 レイヤ × (ミップ 5 段 + サムネイル 1)
                 ExternalChunk わずか 4 件
```

#### 足すときに生成が要るもの / 要らないもの

**縮小ミップ段の画素は作らなくてよい** [実測: 本番 3 ファイルすべてで
50% 以下のミップ段は 0/69・0/208・0/452 と、一つも実体を持たない]。
CSP は SQLite 上に連鎖を宣言するが、画素は **100% 段とサムネイルにしか書かない**。

| 対象 | 必要な作業 |
|---|---|
| 100% ミップ Offscreen | `Attribute` + チャンク本体。**ここが実質すべて** |
| サムネイル Offscreen | 全レイヤが実体を持つ (69/69, 207/207, 450/450) ので必要 |
| 縮小ミップ段 (50%〜) | SQLite 行 + `Attribute` のみ。`BlockSize[]` は全部 104、チャンクなし |
| `Layer` 行 | 親の子チェーン (`LayerFirstChildIndex`/`LayerNextIndex`) に繋ぐ |
| `Mipmap` / `MipmapInfo` / `LayerThumbnail` | 連鎖を張る (描画用とマスク用は別連鎖) |
| `ExternalChunk` | 実際に書いたチャンクの分だけ |
| `ElemScheme.MaxIndex` | **MainId の採番元**。SQLite の AUTOINCREMENT ではない |
| `sqlite_sequence` | `_PW_ID` の AUTOINCREMENT 台帳。行数と一致する |

`ElemScheme.MaxIndex` は「これまでに払い出した最大 ID」の水位で、削除しても下がらない
[実測: emptyimage は Layer 最大 MainId=4 / MaxIndex=4、tama は 69 / 71]。
採番は `new_id = MaxIndex + 1` してから `MaxIndex` を更新する。

**空レイヤは「チャンクを省略」ではなく「全ブロックが空のチャンク」として書かれる**
[実測: emptyimage の 'レイヤー 1' の 100% ミップは 35 ブロック全部が `BlockSize=104`
でありながらチャンク実体を持つ]。この作法に倣うのが安全。

#### キャンバスサイズの差し替えは現実的

用紙レイヤとルートフォルダの Offscreen は **初期色のみで画素を持たない** ので、
`Attribute` の `width/height/cols/rows/BlockSize[]` を作り直すだけでサイズを変えられる。
画素の再生成は要らない。

ただし `Canvas.CanvasWidth/Height` は **`CanvasUnit` の単位であってピクセルとは限らない**
[実測: haruse は unit=2 (mm) の 257x364mm @600dpi = 6071x8598 px]。
実ピクセル寸法はルートフォルダの 100% ミップの `Attribute` から取ること。

#### 最大の落とし穴: `Layer` のスキーマはファイルごとに違う

CSP は **その文書で使っている機能に応じて列を作る** [実測]:

```
emptyimage.clip   56 列
emptyanime.clip   57 列   差分はちょうど 1 列 = AnimationFolder
tama.clip         52 列   emptyimage に無い FilterLayerInfo を持つ (色調補正レイヤがある)
nazoani01_ja.clip 113 列  emptyimage に無い列が 59 個 (Camera2D*, Audio*, アニメセル関連…)
```

つまり **空っぽすぎるテンプレートは、後から足したい機能の列を物理的に持っていない**。
`ALTER TABLE ADD COLUMN` は SQLite 的には通るが、CSP が期待する型/既定値は推測になる。

→ **テンプレートは「作りたいものに一番近い状態で CSP に保存させたファイル」を用途別に
用意する**のが正解。テキストレイヤを足したいならテキストレイヤ入りのファイルを、
アニメを作るなら `emptyanime.clip` を種にする。

#### 「既存に足す」と「テンプレートから作る」は同じ実装になる

テンプレート方式は「空という名の既存ファイルに足す」に過ぎないので、
**テンプレート専用のコードを書く必要はない**。append 実装を 1 本作れば、
テンプレートはその入力データでしかない。

---

## 7. 変換処理 (完全でなくてよい、の範囲)

### 7.1 CLIP → PSD (実現性: 高)

必要なもの:

- レイヤツリー → PSD のフォルダ区切り (`lsct`) 変換 … 機械的
- 画素 (RGBA ストレート) → PSD のチャンネル分解 + PackBits … psdparse の
  `setLayerPixels` / `addLayer` がそのまま使える (入力は BGRA インターリーブ)
- 合成モード §5.3 の表で写像、非対応 2 種は近似
- 不透明度 0..256 → 0..255
- マスク → `setLayerMaskPixels`
- 用紙レイヤ → 不透明白のベタレイヤとして出力
- ベクタ/テキスト/3D → **ラスタライズ済み offscreen をそのまま画素として出力**
  (CSP が保存時に描いたキャッシュがあるので実用上これで足りる)

**psdparse を書き出し側にそのまま使えるのが大きい**。
`clip → (共通中間表現) → psdparse::addLayer` で最短経路が引ける。

想定される欠落: レイヤ効果、定規、タイムライン、ベクタの編集可能性、
グループのキャッシュしか無いケース (子が空になる)。

### 7.2 PSD → CLIP (実現性: 中〜低)

- 画素側は「256x256 タイル + アルファ面折り畳み + zlib」で書ける
  (`encode_pixel_block` 相当は実装済みの参照がある)
- 問題は **SQLite を一から組み立てる**こと。`Layer` は 200 列近くあり、
  CSP が期待する既定値が分からない列が多い。`ParamScheme` / `ElemScheme` も要る。
- 現実解: **既存の .clip をテンプレートとして開き、レイヤを足し引きする**。
  ゼロから生成しない。

### 7.3 CLIP → PNG/連番 (実現性: 高、最初の実用出口)

レイヤごと PNG + `layers.json` 出力。psdparse の `tools/psd_export.py` と同じ形。
合成は Pillow / 自前どちらでも。**ここを最初のマイルストーンにするのが良い**。

---

## 8. 実装ロードマップ案

| Phase | 内容 | 検証方法 |
|---|---|---|
| P0 | 調査・仕様書 (本ドキュメント群) | 実ファイルで数値一致 ✅ |
| P1 | C++: チャンク走査 + SQLite deserialize + `Layer` ツリー + `Canvas` | ダンプ CLI が clip-tools と一致 |
| P2 | `Offscreen.Attribute` パース + ブロック遅延展開 + `layerImage()` | `test000.png` とピクセル一致 (Python 版で既に達成) |
| P3 | 部分読み `layerRegion()` / ミップ段指定 | 全面読みと部分読みが一致 |
| P4 | 共通 API 面 (`imgdoc::Document`) を切り、psdparse 側にも実装 | 同じテストコードが両形式で通る |
| P5 | Python バインディング (pybind11、psdparse と同じ流儀) | |
| P6 | CLIP → PSD 変換 (psdparse を書き出しに使う) | 往復目視 + 合成差分 |
| P7 | W1 (属性編集) → W2 (画素差し替え) → W3 (レイヤ追加) | **CSP で開けること** |

P7 は **ファイルを読むだけでは正しさを検証できない唯一のフェーズ**。CSP 実機が要る。
段取りは必ずこの順で:

1. **無変更の読み書き往復でバイト一致** ← 土台。ここが通らないうちは先へ進まない
2. **属性だけ変更** (不透明度など) — SQLite の UPDATE のみ。オフセット連鎖に触らないので最も安全
3. **画素差し替え** — チャンク再配置 + `ExternalChunk.Offset` 更新が発生
4. **レイヤ追加** — §6.1

2 が CSP で開ければ「CSP が我々の書いた SQLite を受け入れる」ことの確証になる。

P2 の正しさは Python プロトタイプ (`tools/clip_lazy_demo.py`) で
**既に max diff = 0 で実証済み**なので、C++ 移植はアルゴリズムの写経で済む。

---

## 9. リスクと未確定事項

1. **カラーモードの網羅性**。4 サンプルすべて 8bit RGBA (`CanvasChannelBytes = 1`)。
   16bit / CMYK / グレースケール文書は未検証で、`color_mode` の値の意味も未確定。
2. **CSP のバージョン差**。`Layer` の列数がサンプル間で 52 / 106 / 113 とばらつく。
   `ExternalTableAndColumnName` が存在しないテーブルを指すのも常態。
   **動的テーブルビューを基層にする方針はこのリスクへの直接の回答**。
3. **ベクタ / テキスト / binc の中身**は未着手。ただしラスタ化済み offscreen が
   あるので、読み出しと PSD 変換には当面必要ない。
4. **sqlite3 への依存**が増える。amalgamation を同梱すれば外部パッケージは不要
   (zlib と同じ扱いにできる)。
5. **書き込みは offset 連鎖のため片手間にはできない**。読みと変換を先に固める。
6. **合成の再現は別問題**。本調査で確定したのは「正しい画素を取り出せる」ところまで。
   合成モード 27 種・クリッピング・マスク・パススルー・色調補正レイヤを CSP と
   一致させるのは独立した工数。

   **正解画像は全ファイルに入っている**: `CanvasPreview.ImageData` は PNG で、
   キャンバスの完成画そのもの [実測: tama は 1447x2046 (1/2)、haruse は 1517x2149 (1/4)、
   nazoani は 1152x815 (等倍)、emptyimage は真っ白 255 単色]。縮尺は
   長辺で頭打ちになる模様。

   現状の素朴な合成 (通常合成のみ、不透明度も未適用) と tama のプレビューの差は
   `mean=87 / 255`。ただしこれは「最前面の テクスチャ レイヤ (overlay 45%) を
   不透明の通常合成で塗って全部覆ってしまう」ためで、出力は 152 色・std 12 の
   ほぼ単色。**合成は未着手であることを示す数字であって、詰めの難易度を示す数字ではない。**
