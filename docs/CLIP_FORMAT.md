# CLIP (CLIP STUDIO PAINT) ファイル形式 仕様メモ

出典と検証状況:

- **一次資料**: [animeops/clip-tools](https://github.com/animeops/clip-tools) のソース解析
  (特に `clip_tools/clip.md`, `structs/chunk.py`, `structs/offscreen_attributes.py`,
  `structs/encode_blocks.py`, `sqlite_records.py`)。
- **本ドキュメントの追検証**: 以下 4 ファイルを独自に読み直して確認した。
  バイト位置・フィールド意味は実測で裏を取っている。
  検証コードは `tools/clip_probe.py` / `tools/clip_lazy_demo.py`。

  | ファイル | サイズ | レイヤ | Offscreen | 特徴 |
  |---|---|---|---|---|
  | `test000.clip` (clip-tools 同梱) | 1.1 MB | 3 | 24 | 参照 PNG 付き |
  | `samples/tama.clip` | 60 MB | 65 | 488 | 色調補正レイヤ + マスク + フォルダ |
  | `samples/nazoani01_ja.clip` | 63 MB | 452 | 3144 | `Track` / `TimeLine` (binc) |
  | `samples/haruse-ja.clip` | 91 MB | 206 | 1877 | ベクタ + テキスト + ブラシ + 定規 |

  ブロックサブレコードは合計 **164,562 個**を走査し、後述のアサーションが全て成立した。
- 記号: **[実測]** = 本リポジトリで実ファイル確認済み / **[推定]** = clip-tools 側の
  推測ラベルをそのまま引いており未確認。

clip-tools のパーサが持っていた理解より踏み込めた点が 2 つある (後述):

1. ブロックサブレコードは **サイズ前置** であり、`Offscreen.Attribute` の
   `BlockSize[i]` は「圧縮サイズ」ではなく **サブレコード全体のバイト数** である。[実測]
2. したがって `ExternalChunk.Offset` + `BlockSize[]` の累積和だけで、
   **バイナリ領域を一切走査せずに任意ブロックの絶対オフセットが求まる**。[実測]

これは psdparse 型の遅延読み (メタ情報のみ保持し、実データは元ファイルから都度取得)
を CLIP で成立させる決定的な性質。詳細は [DESIGN.md](DESIGN.md)。

---

## 1. ファイル全体構造

```
+----------------------------------------------------------+
| CSFCHUNK ヘッダ (24 bytes)                                |
+----------------------------------------------------------+
| CHNKHead                                                  |
| CHNKExta  (外部チャンク 0..N: 実ピクセル/ベクタ/3D データ)|
| CHNKSQLi  (SQLite3 データベース = 全メタ情報)             |
| CHNKFoot                                                  |
+----------------------------------------------------------+
```

全整数は特記なき限り **ビッグエンディアン**。文字列マーカーは **UTF-16BE**。
(唯一の例外: ブロックの zlib 圧縮長のみリトルエンディアン。後述。)

### 1.1 ファイルヘッダ [実測]

| offset | size | 内容 |
|---|---|---|
| 0 | 8 | `"CSFCHUNK"` |
| 8 | 8 | u64 ファイル全体サイズ (= 実ファイルサイズと一致) |
| 16 | 8 | u64 ヘッダ長 (= 24) |

### 1.2 チャンク共通ヘッダ [実測]

```
u64 相当: char[8] chunk_type
u64       chunk_length     # type/length 自身を含まない本体長
```

| chunk_type | 意味 |
|---|---|
| `CHNKHead` | ファイルヘッダ本体 (40 bytes) |
| `CHNKExta` | 外部チャンク (実データ 1 件) |
| `CHNKSQLi` | SQLite データベース |
| `CHNKFoot` | 終端 (length = 0) |

**注意**: `CHNKExta` の `chunk_length` は「56 + data_size」であり、次チャンクへは
`pos + 16 + 56 + data_size` で進む。clip-tools は length を使わず内部ヘッダから
歩を進めているが、どちらでも同じ位置になる。[実測: 584 = 56 + 528]

### 1.3 CHNKHead 本体 (40 bytes) [実測]

```
u64 version               # 256 (=0x100)
u64 binary_section_size   # CHNKSQLi の開始オフセットと一致 (= バイナリ領域長)
u64 identifier_length     # 16
u8[16] identifier         # ファイル固有 UUID 相当
```

### 1.4 CHNKExta ヘッダ [実測]

```
u64    external_id_length   # 40
char[40] external_id        # ASCII "extrnlid" + 32桁 HEX
u64    data_size            # 続くペイロード長
u8[data_size] payload
```

ペイロード先頭 = `chunk_offset + 16 + 56`。

### 1.5 SQLite 領域

`CHNKSQLi` の本体がそのまま SQLite3 データベースファイル。先頭は
`"SQLite format 3\0"`。clip-tools はこのマジックを `find()` して分割しているが、
**チャンクを正しく歩けば offset/length が確定する**ので走査は不要 (かつ安全)。[実測]

さらに、**`CHNKHead.binary_section_size` が `CHNKSQLi` チャンクヘッダの位置と一致する**
[実測: 4 ファイル全て]。つまり先頭 64 バイトだけ読めば SQLite に直行でき、
チャンク走査すら要らない。走査は整合性チェックとして残せばよい。

---

## 2. SQLite スキーマ (メタ情報の本体)

`test000.clip` に存在したテーブル (17):

| テーブル | 役割 |
|---|---|
| `Canvas` | キャンバス寸法・解像度・ルートフォルダ ID・コミック設定 |
| `Project` | プロジェクトメタ |
| `Layer` | レイヤツリーと全レイヤ属性 (200 列近い) |
| `Offscreen` | ラスタ実体 1 枚分の記述 (ジオメトリ + ブロック表 + 外部 ID) |
| `Mipmap` / `MipmapInfo` | レイヤごとのミップマップ連鎖 |
| `LayerThumbnail` | レイヤサムネイル |
| `ExternalChunk` | **external_id → バイナリ領域内オフセット** |
| `ExternalTableAndColumnName` | external_id を保持する (table, column) の一覧 |
| `CanvasPreview` | キャンバス全体のプレビュー画像 (**開いた直後に表示される**、下記) |
| `AnimationCutBank`, `CanvasItem(Bank)`, `RemovedExternal` | アニメ/素材/削除済み管理 |
| `ParamScheme`, `ElemScheme` | CSP の UI 用スキーマ定義 (描画には無関係。1179 行など巨大) |
| `sqlite_sequence` | SQLite 内部 |

実運用ファイルではさらに `VectorObjectList`, `BrushStyle`, `BrushPatternImage`,
`Track`, `TimeLine`, `LayerComp`, `CameraInfo`, `Canvas3DModelBank`,
`RulerPerspective`, `SmallObjectInfo`, `TimeLapseBlob` などが出現する
(clip-tools `sqlite_records.py` に型付きビューあり)。

### 2.0.1 `CanvasPreview` [実測]

```
CanvasPreview(MainId, CanvasId, ImageType=1, ImageWidth, ImageHeight, ImageData)
```

`ImageData` は**キャンバス全体の PNG (RGBA)**。全サンプルで 1 行だけ持つ。

**CSP は開いた直後この画像を表示する** [実測: WRITE_TEST_4 ④]。書き換えた
ファイルでここを古いまま残すと、**開いた直後だけ違う絵が出て、レイヤを
操作すると正しくなる**という挙動になる。書く側は合成し直して差し替えること
(`clip_build.set_canvas_preview` / `clip_write.refresh_preview`)。

### 2.1 external_id の解決 [実測]

```
ExternalTableAndColumnName(TableName, ColumnName)
    → 「この列に external_id が入る」宣言のリスト
ExternalChunk(ExternalID, Offset)
    → 「その external_id は バイナリ領域のこのオフセットにある」
```

`test000.clip` の `ExternalTableAndColumnName`:

```
Offscreen.BlockData          ← ラスタ画素 (本命)
VectorObjectList.VectorData  ← ベクタストローク
Canvas3DModelBank.BankData
Canvas3DModelLoader.ModelData
Track.TrackActionMixer / TrackActionMixer2   ← binc (アニメーション)
CanvasItemBinary.ItemBinaryData
Manager3DOd.SceneData
ModelData3D.Layer3DModelData
TimeLapseBlob.BlobData
```

**重要な非対称性** [実測]: `Offscreen` は 24 行あるのに `ExternalChunk` は 6 行しかない。
つまり **SQLite 上で宣言された Offscreen の大半は、バイナリ領域に実体を持たない**。

- `ExternalChunk` に載っている external_id = 実体があるもの、が正確に一致する
  (6 個の `CHNKExta` と 1:1)。
- 実体を持たない Offscreen は「初期色のみ (画素不要)」か「CSP が保存時に
  親グループのキャッシュへ焼き込んで空にした」ケース。

`ExternalTableAndColumnName` は **このファイルに存在しないテーブル**も列挙する [実測:
test000 では 10 行中 9 行が該当テーブルを持たない]。列が存在しないケースも報告されている
(スキーマ版差)。パーサは欠損テーブル/列を黙ってスキップする必要がある。

**`Attribute.BlockSize[]` は実体の有無を示さない** [実測]。ルートフォルダの 100% ミップ
(Offscreen #55) は `[2362, 27976, 6480, ...]` という「中身のあるサイズ」を持つのに、
対応する `CHNKExta` は存在しない。過去に書かれたキャッシュのサイズ表が残っているだけ。
**実体の有無は `ExternalChunk` に載っているかどうかだけで判定すること。**

### 2.2 Layer テーブル (ツリーと属性)

ツリーは配列ではなく **リンクリスト**:

```
Layer.LayerFirstChildIndex → 最初の子の MainId
Layer.LayerNextIndex       → 次の兄弟の MainId
Canvas.CanvasRootFolder    → ルートフォルダの MainId
```

`test000.clip` の実測:

```
MainId=2 name=''        LayerType=256  LayerFolder=1  FirstChild=4  Next=0   ← ルート
MainId=4 name='用紙'    LayerType=1584 LayerFolder=0  Next=3
MainId=3 name='layer1'  LayerType=1    LayerFolder=0  Next=5
MainId=5 name='layer2'  LayerType=1    LayerFolder=0  Next=0
```

**描画順**: 子チェーンの先頭が最下層 (用紙 → layer1 → layer2 の順に上へ重なる)。[実測]

主な列:

| 列 | 意味 |
|---|---|
| `MainId` | レイヤ ID (Offscreen.LayerId 等から参照される) |
| `LayerName` | 名前 (UTF-8) |
| `LayerType` | 種別。`LayerKind`: 0=その他, 1=ラスタ, bit1(=2)=マスク有, 256=ルート, 512=2Dカメラ, 1584=用紙, 4096=フィルタ |
| `LayerFolder` | bit0 = フォルダ, bit4 = 折り畳み |
| `LayerVisibility` | 0/1 |
| `LayerComposite` | 合成モード (`LayerComposite` enum: 0=通常, 2=乗算, 30=通過, 36=除算 …) |
| `LayerOpacity` | **0..256** (255 ではない) |
| `LayerClip` | 非 0 = 下のレイヤでクリッピング |
| `LayerLock` | bit0 = 編集ロック, bit4 = 透明ピクセルロック |
| `LayerOffsetX/Y`, `LayerRenderOffscrOffsetX/Y`, `LayerMaskOffsetX/Y` | 各種オフセット |
| `LayerRenderMipmap`, `LayerRenderThumbnail`, `LayerLayerMaskMipmap` | 各テーブルへの FK |
| `TextLayerString`, `TextLayerAttributes`, `TextLayerAddAttributesV01` | テキストレイヤ |
| `ResizableImageInfo`, `Camera2D*` | 変形 (ホモグラフィ) 情報 |
| `MonochromeFillInfo`, `LightTableInfo`, `FilterLayerInfo` | モノクロ塗り/ライトテーブル/調整レイヤ |

**オフセットの罠** [clip-tools 実績]: `DrawToRenderOffscreenType` が NULL でない場合、
`LayerRenderOffscrOffsetX/Y` は既に offscreen へ焼き込み済みなので二重加算してはならない。

### 2.2.1 外部チャンク ID の格納型 [実測: 書く側で最重要]

同じ 40 文字の ID (`extrnlid` + 32 桁の大文字 16 進) が、**テーブルごとに違う
格納型で入っている** [実測: 33 ファイル・7,000 行超で例外なし]:

| 列 | 宣言 | 実際の `typeof()` |
|---|---|---|
| `Offscreen.BlockData` | BLOB | **`blob`** |
| `ExternalChunk.ExternalID` | BLOB | **`text`** |

**どちらを取り違えても CSP は黙って壊れる**:

- `Offscreen.BlockData` に TEXT を入れると、**CSP は実体を見つけられず
  そのレイヤを全面透明として開く** (エラーも出ない)
- `ExternalChunk.ExternalID` に bytes を束縛して `UPDATE` すると
  **1 行もマッチせず**、オフセットが更新されないままのファイルができる

SQLite は宣言型に関係なく束縛した値の型で格納するので (型親和性)、
どちらも構文としては通ってしまう。**自前のリーダは両方読めてしまうため、
実機で開くまで気付けない**。実際 W3 (レイヤ追加) と W4 (PSD→CLIP) は
これで「レイヤはあるが中身が透明」になっていた。

### 2.3 Offscreen / Mipmap / MipmapInfo / LayerThumbnail [実測]

```
Offscreen(MainId, CanvasId, LayerId, Attribute BLOB, BlockData external_id)
Mipmap(MainId, CanvasId, LayerId, MipmapCount, BaseMipmapInfo→MipmapInfo.MainId)
MipmapInfo(MainId, CanvasId, LayerId, ThisScale, Offscreen→MainId, NextIndex→MipmapInfo.MainId)
LayerThumbnail(MainId, CanvasId, LayerId, ThumbnailOffscreen→Offscreen.MainId, ...)
```

**レイヤは最大 2 本のミップ連鎖を持つ** [実測]。ここを取り違えるとマスクを
レイヤ画像として読んでしまう:

```
Layer.LayerRenderMipmap      → Mipmap.MainId → BaseMipmapInfo → MipmapInfo 連鎖 (描画用)
Layer.LayerLayerMaskMipmap   → Mipmap.MainId → BaseMipmapInfo → MipmapInfo 連鎖 (レイヤマスク用)
Layer.LayerRenderThumbnail   → LayerThumbnail.MainId → ThumbnailOffscreen (描画用サムネ)
Layer.LayerLayerMaskThumbnail→ LayerThumbnail.MainId → ThumbnailOffscreen (マスク用サムネ)
```

**`MipmapInfo` を `(LayerId, ThisScale=100.0)` で引いてはいけない** [実測]。
マスク付きレイヤでは描画用とマスク用の**両方**が同じ `LayerId`・同じ `ThisScale=100.0`
で始まるため区別が付かない。必ず `Layer.LayerRenderMipmap` → `Mipmap.BaseMipmapInfo`
から辿り、`MipmapInfo.NextIndex` で降りること。

tama.clip のレイヤ 61 (色調補正レイヤ):

```
Mipmap 63 (描画) LayerId=61 MipmapCount=6 BaseMipmapInfo=68
Mipmap 64 (マスク) LayerId=61 MipmapCount=7 BaseMipmapInfo=69
MipmapInfo 68 ThisScale=100.0 Offscreen=130 Next=377   ← 描画用
MipmapInfo 69 ThisScale=100.0 Offscreen=132 Next=382   ← マスク用 (同じ LayerId/Scale!)
```

段数は 5 段固定ではない [実測]: `ThisScale` は 100 / 50 / 25 / 12.5 / 6.25 / 3.125 /
1.5625 まで観測 (`MipmapCount` = 5〜7)。

**段数と寸法の決まり方** [実測: 5 ファイルで一致]。100% から `//2` で縮小し、
**ブロックグリッドが 1x1 になった段の、さらに次の段まで**作る:

```
1400x700 -> 700x350 -> 350x175 -> 175x87 -> 87x43     グリッド 6x3,3x2,2x1,1x1,1x1
1600x1200-> 800x600 -> 400x300 -> 200x150 -> 100x75   グリッド 7x5,4x3,2x2,1x1,1x1
 800x1000-> 400x500 -> 200x250 -> 100x125             グリッド 4x4,2x2,1x1,1x1
 300x400 -> 150x200 -> 75x100                         グリッド 2x2,1x1,1x1
```

サムネイルの Offscreen は**キャンバス寸法によらず 512x512 (2x2) 固定** [実測]。
書く側 (`tools/clip_build.py`) はこの規則でミップ連鎖を組み立てる。

**`Mipmap.MipmapCount` は必ず連鎖の段数と一致させること** [実測: CSP 5.0.4]。
段数を減らしたのに `MipmapCount` を放置したファイルは、**CSP が読み込み中に
落ちる** (存在しない段まで辿ろうとする)。`NextIndex = 0` で止まる実装からは
見えないので、書く側は必ず更新する。

**サムネイルの作り直し**:

- 実体 (`CHNKExta`) を**消す**と、CSP は開いた時に作り直す [実測: WRITE_TEST ③]
- ただし実体を消すだけでは足りず、**`LayerThumbnail` の `Thumbnail*NeedRefresh`
  を更新しないと古い絵が残る** [実測: WRITE_TEST_4 ②]。この列は 0/1 のフラグ
  ではなく**世代番号**らしく、実測値は 0〜380 万まで散らばる。CSP が新しく
  足したレイヤでは **50**、既存レイヤでは 5 だった
  [実測: `samples/addlayer_csp.clip`]。書く側は 50 を入れている

Offscreen の役割は 3 種類ある [実測]。サムネイルは「`MipmapInfo` に載っていないもの」
ではなく `LayerThumbnail` 経由で引くこと。

1. **ミップ段** — `MipmapInfo.Offscreen` から指される
2. **サムネイル** — `LayerThumbnail.ThumbnailOffscreen` から指される
3. **どちらからも指されない Offscreen** — テキスト/ベクタ等のオブジェクトレイヤが持つ
   **レンダリング結果のタイトな bbox ラスタ**。どのテーブルの FK からも参照されず、
   `Offscreen.LayerId` でしか辿れない

多くのファイルでは 3 が存在せず「Offscreen 総数 = ミップ段 + サムネイル」が成り立つが、
**一般には成り立たない** [実測]:

```
                Offscreen   ミップ段   サムネ   どちらでもない
tama.clip             488       419      69       0
test000.clip           24        20       4       0
text.clip              22        15       5       2 (全て実体あり)
haruse-ja.clip       1877      1664     207       6 (全て実体あり)
nazoani01_ja.clip    3144      2263     450     431 (全て実体あり)
```

`text.clip` の例: テキストレイヤ 2 枚の 100% ミップは**実体を持たず**、
代わりに 31x151 / 194x35 という**文字列の外接矩形サイズの Offscreen** が実体を持つ。
つまり **テキストレイヤはキャンバス全面のラスタを保存しない**。

**配置位置は `TextLayerAttributes` の TLV タグ 42 (`text_bbox`)** [実測]。
BLOB 末尾は `u32 LE tag + u32 LE length + value` の TLV 列で、タグ 42 の値は
`i32 LE × 4 = (x0, y0, x1, y1)`。この `(x0, y0)` にオブジェクト Offscreen を
そのまま置くと **CanvasPreview とピクセル完全一致する** (`text.clip` で max=0)。

- 幅は `x1 - x0 == offscreen_width - 1` で両サンプルとも一致した。
- **高さは一致しないことがある** (offscreen 35 に対し `y1-y0 = 30` など。行送り分?)。
  TLV の開始位置を求めるには手前のセクションを全部解く必要があるため、
  `tools/clip_lazy_demo.py` では「タグ 42・長さ 16・幅が Offscreen と一致」で
  同定している。

**ベクタレイヤは外接矩形ラスタすら持たない** [実測: `vector.clip` のベクタレイヤは
サムネイルのみ]。ストロークは `VectorObjectList.VectorData` にあるので、
描画にはブラシエンジンの実装が要る。

test000 では 1 レイヤあたり Offscreen 6 行 (ミップ 5 段 + サムネイル 512x512 1 枚)。

実体 (`ExternalChunk`) を持っていたのは以下だけ [実測]:

| Offscreen | Layer | 種別 | 実体 |
|---|---|---|---|
| 60 | 2 (ルートフォルダ) | サムネイル 512x512 | あり (全ブロック空) |
| 66 | 4 (用紙) | サムネイル 512x512 | あり (全ブロック空) |
| 67 | 3 (layer1) | ミップ 100% 1400x700 | **あり (画素本体)** |
| 72 | 3 (layer1) | サムネイル | あり |
| 73 | 5 (layer2) | ミップ 100% 1400x700 | **あり (画素本体)** |
| 78 | 5 (layer2) | サムネイル | あり |

つまり:

- **用紙レイヤの 100% ミップは実体を持たない**。`Attribute` の InitColor
  (不透明白) だけで全面が決まるため。[実測]
- **グループ (フォルダ) の 100% ミップも実体を持たないことが多い**。キャッシュされた
  512x512 サムネイルだけが唯一のレンダ済みコピー、というケースがある。[clip-tools 報告]
- さらに、キャッシュ対象グループ配下の葉ラスタが **どの Offscreen にも実体を持たない**
  ことがある (CSP が保存時にグループのキャッシュへ焼き込む)。子を辿るだけの
  コンポジタは空になるので、グループキャッシュへのフォールバックが要る。
  [clip-tools 報告 / 本サンプルでは未再現]

---

## 3. Offscreen.Attribute BLOB

自己記述的な TLV 風コンテナ。3 セクション (`Parameter` / `InitColor` / `BlockSize`)
が `9` という u32 境界マーカーで区切られ、セクション名は UTF-16BE (9文字=18バイト)。

### 3.1 レイアウト [実測]

```
u32[4] section_sizes
  [0] = 16                    # この表自身の長さ
  [1] = 102                   # Parameter セクション長
  [2] = 42 (has_color なら 58)# InitColor セクション長
  [3] = 34 + 4*nblocks        # BlockSize セクション長
u32    boundary = 9
utf16be "Parameter" (18B)
  u32 width, u32 height       # この offscreen の論理サイズ
  u32 cols,  u32 rows         # ブロックグリッド。nblocks = cols*rows
  u32[4] (33, 1, num_channels, 5)      # [推定] color_mode, alpha_flag, ch数, bit_depth
  u32[4] (65536, 4, 1024, 1)           # [推定] ブロック幾何の定数
  u32[4] (block_w, 65536, block_h, block_stride)
  u32[4] (8, 8, 0, 0)                  # [推定] サブブロック
u32    boundary = 9
utf16be "InitColor" (18B)
  u32    20                            # [推定] セクション種別かボディ長
  u32[4] (has_color, packed_rgba, num_extra, num_channels)
  u32[num_extra] チャンネル別初期値     # num_extra 個。観測値は全ゼロ
u32    boundary = 9
utf16be "BlockSize" (18B)
  u32[3] (12, nblocks, num_channels)
  u32[nblocks] block_sizes
```

`section_sizes` があるので、内部を知らなくてもセクション単位でスキップできる。

### 3.2 実測値 (test000.clip)

```
Offscreen 67 (layer1, 100% ミップ):
  sections=(16, 102, 42, 106)
  w=1400 h=700 cols=6 rows=3          → nblocks=18
  colormode=(33,1,4,5) geom=(65536,4,1024,1) dims=(256,65536,256,256) sub=(8,8,0,0)
  InitColor: magic=20, (has=0, 0, 0, 4)
  BlockSize: magic=12 nblocks=18 nchan=4
             [2126,18538,4975,104,104,104,9160,41798,36796,30761,
              14068,104,104,3880,9147,14975,2143,104]

Offscreen 61 (用紙, 100% ミップ, 実体なし):
  InitColor: magic=20, (has=1, 0xFFFFFFFF, 4, 4)  extra=(0,0,0,0)
  BlockSize: 全 18 ブロックが 104 (= 空ブロックのレコード長)
```

`packed_rgba` は **RGBA の big-endian パック** (`0xFFFFFFFF` = 不透明白)。[実測: 用紙が白で一致]

**`InitColor` の末尾は可変長** [実測]。`has_color == 1` を見て 16 バイト固定で読むと壊れる:

```
section_sizes[2] == 42 + 4 * num_extra
```

- RGBA 面: `(1, 0xFFFFFFFF, 4, 4)` → 追加 16B、`section_sizes[2] = 58`
- 単一プレーン面 (マスク/選択、`num_channels == 0`): `(1, 0xFFFFFFFF, 0, 4)` →
  **追加なし**、`section_sizes[2] = 42`

clip-tools の `process_offscreen_attributes` はここで
`Invalid attribute: missing BlockSize header` を投げる (tama.clip で 14/488 件)。
詳細は [CLIP_TOOLS_REPORT.md](CLIP_TOOLS_REPORT.md) §1。

### 3.2.1 表現色とプレーン構成 [実測: グレー/モノクロのサンプルで確定]

`Parameter` セクションの `(color_mode, alpha_flag, num_channels, bit_depth)` は
定数ではなく、**面の表現色と連動する**。観測された 4 通り (256x256 ブロック):

| color_mode | nch | bit | plane_bytes | 展開後 | 構成 |
|---|---|---|---|---|---|
| 33 | 4 | 5 | 65,536 | 327,680 | `(bh+64, bw, 4)`。rows[64:]=B,G,R,未使用 / rows[0:64]=折り畳み α |
| 17 | 1 | 2 | 65,536 | 131,072 | 8bpp 2 面。**plane0 = α, plane1 = グレー値** |
| 17 | 1 | 1 | 8,192 | 16,384 | 1bpp 2 面。**plane0 = α, plane1 = 値**。行 = `row_bytes`(=32) |
| 1 | 0 | 1 | 0 | 65,536 | 8bpp 1 面のみ (マスク / 選択範囲)。幾何フィールドはゼロ |

`Canvas` 側とも対応する [実測]:

```
CanvasDefaultColorTypeIndex : 0=カラー / 1=グレー / 2=モノクロ  (新規作成の「基本表現色」)
CanvasDefaultChannelOrder   : 33=カラー / 17=グレー・モノクロ   (Attribute の color_mode と同値)
```

**表現色はレイヤのラスタにのみ効く** [実測]。グレー/モノクロ文書でも
**用紙・ルートフォルダ・全サムネイルは RGBA (33,1,4,5) のまま**。
またモノクロ文書でも、縮小ミップ段は 8bpp グレー `(17,1,1,2)` で保持される
(1bpp なのは 100% 段だけ)。

展開後サイズは統一則 **`(plane_count + 1) * plane_bytes`** で表せる
[実測: 16,916 ブロック中 16,814 で成立]。例外はマスク面 (`nch=0`) のみで、
そちらは幾何フィールドがゼロに潰されている。
**実装はサブレコードヘッダの `decompressed_size` を直接使うのが安全**で、
この統一則は整合性チェックとして使う。

> clip-tools の `layer_blocks.py` は `num_channels == 1` を「ブラシ?」として
> plane0 を値、plane1 を捨てて 255 で埋めている。**実際は逆で plane0 が α、
> plane1 が値**。詳細は [CLIP_TOOLS_REPORT.md](CLIP_TOOLS_REPORT.md) §6。

### 3.3 `BlockSize[i]` の正体 [実測 / clip-tools の理解を訂正]

clip-tools の `clip.md` は「各ブロックの圧縮バイト数」としているが、実際は
**チャンクストリーム内のサブレコード全長**である。実測で完全一致:

```
Attribute の block_sizes = [2126, 18538, 4975, 104, 104, ...]
チャンク内の実レコード長 = [2126, 18538, 4975, 104, 104, ...]   ← 完全一致
```

したがって:

```
block[i] のチャンク内相対オフセット = sum(block_sizes[0..i-1])
空ブロック                          = block_sizes[i] == 104
zlib 圧縮長                          = block_sizes[i] - 112
```

「一様色ブロックは ~104 バイトに圧縮される」という記述も同様に訂正で、
104 バイトは **画素を 1 バイトも持たない空レコードの固定長**である。

---

## 4. チャンクストリーム (CHNKExta ペイロード)

### 4.1 ブロックサブレコード [実測]

```
u32 BE  record_size          # このフィールドを含む全長。= Attribute.BlockSize[i]
u32 BE  name_length = 19
utf16be "BlockDataBeginChunk"                     (38 B)
u32 BE  block_index                               # 行優先 (row-major)
u32 BE  decompressed_size                         # = (block_h + 64) * block_w * 4
u32 BE  block_width  = 256
u32 BE  block_height = 256
u32 BE  has_content                               # 0 = 空ブロック
   has_content != 0 のとき:
     u32 BE  section_size    # = compressed_size + 4
     u32 LE  compressed_size # ★ここだけリトルエンディアン
     u8[compressed_size] zlib ストリーム
u32 BE  name_length = 17
utf16be "BlockDataEndChunk"                       (34 B)
```

サイズ検算 [実測: 全ブロックで成立]:

```
空          : 4 + 4 + 38 + 20            + 4 + 34 = 104
データあり  : 4 + 4 + 38 + 20 + 8 + clen + 4 + 34 = clen + 112
```

`decompressed_size` は `(256+64)*256*4 = 327680 = 0x50000` で全ブロック一定 [実測]。
zlib 展開後の長さがこの値と一致することも確認済み。

### 4.2 末尾レコード (サイズ前置なし) [実測]

ブロック列の後ろに以下が続く。**`record_size` 前置がなく、いきなり name_length で始まる**
点がブロックレコードと違う (clip-tools のヒューリスティックはこの差を吸収している)。

```
u32 BE  name_length
utf16be "BlockStatus" (11) / "BlockCheckSum" (13)
u32 BE  12                  # ヘッダ長
u32 BE  count               # = nblocks
u32 BE  4                   # 1 エントリのバイト幅
u32[count] エントリ
```

test000 の layer1 では 224 バイト = BlockStatus(110) + BlockCheckSum(114)。
`BlockStatus` は全ブロック 1 [実測: 全サンプル]。

**`BlockCheckSum` の算法は未特定。ただし 0 なら CSP は検査しない** [実測: CSP 5.0.4]。

同じ画素を 3 通りの書き方で作って CSP に開かせた結果:

| 書いた値 | CSP の反応 |
|---|---|
| **0** | **正常に開く**。画素も正しい |
| CRC32 (算法違い) | **「レイヤ画像またはレイヤーマスクが破損しています」**。開くが画像が壊れる |
| 欄ごと省略 | 正常に開く |

つまり **CSP は非ゼロの検査値を実際に照合していて、0 は「検査値なし」の扱い**。
書く側は **0 を書けばよい** (算法を解かなくても実用になる)。

値そのものは相変わらず分からないが、**ゼロかどうかは画素の有無と完全に一致する**:

| | |
|---|---|
| `has_content = 1` のブロック | 検査値が**必ず非ゼロ** (17,185 / 17,185) |
| `has_content = 0` のブロック | 検査値が**必ずゼロ** (148,874 / 148,874) |

試して**外れた**もの (展開後 / 圧縮後 / レコード全体の 3 通りすべてに対して):
CRC32、その 1 の補数、Adler32、Fletcher32、sum32 (BE/LE)、xor32、バイト総和、
FNV-1a、CRC32C。CSP 独自のハッシュと見ている。

書く側の既定は `zero`。`--checksum crc32|none` も残してあるが、
**`crc32` は CSP に拒否されるので使ってはいけない** (切り分け用)。

### 4.3 非ラスタ外部チャンク (binc) [clip-tools 実績]

`Track.TrackActionMixer`, `Canvas3DModelBank.BankData`, `TimeLapseBlob.BlobData` などは
ブロック構造ではなく **フラットな `[u32 LE size][zlib payload]`**。展開すると
Celsys 独自の型付きシリアライズ形式 "binc" になる:

```
"cmt " + version(4) + "binc"      # 12B マジック ("0100" / "0110")
u32 LE  body_crc32                # 以降全バイトの CRC32
u32 LE  num_strings
[u8 len + UTF-8 bytes] * num_strings     # 型名と識別子の共通文字列表
root_node

node:
  (version 0110 のみ) 12B の前方ジャンプ表
  u32 name_idx, u32 type_idx      # type_idx==0 ("null") はコンテナ
  value                           # 型に応じた可変長
  u32 num_attrs, (u32 key_idx, u32 value_idx) * num_attrs
  u32 num_children, node * num_children
```

型: `Byte/SByte/UInt16/Int16/UInt32/Int32/Single/Double/String/Float2/Float3/
Double2/Double3/Quat/Matrix44` とそれぞれの配列版, `Byte[]`, `String[]`。
文字列インデックス `0xFFFFFFFF` は空文字列のセンチネル。

---

## 5. ピクセルブロックのエンコーディング

zlib 展開後のブロックは `(block_h + 64, block_w, 4)` バイト。

```
rows [64:]   カラー部  … B, G, R, (第4チャンネル = 未使用/ゼロ)
rows [0:64]  アルファ面… 256x256 のアルファを 4x4 スーパーピクセルの 64x64 格子に畳み、
                          幅 64 の 4 タイルに分割して格納
```

アルファ復元:

```python
mips  = [img[0:64, 64*k : 64*(k+1)] for k in range(4)]
alpha = (np.concatenate(mips, axis=-1)
           .reshape(64, 64, 4, 4).swapaxes(1, 2).reshape(256, 256))
color[..., 3] = alpha
color[:, :, [0, 2]] = color[:, :, [2, 0]]      # BGR → RGB
```

- **カラー部第 4 チャンネルは意味を持たない** [実測: サンプルでは全ゼロ。
  clip-tools は「エディタが書いた古いバッファ内容」と説明]。
- **アルファはストレート (非プリマルチプライ)** [実測]: 用紙+layer1+layer2 を
  ストレート合成した結果が参照 PNG と **完全一致 (max diff = 0)**。
- `num_channels` による分岐 [clip-tools]:
  - `0`: 単一 8bit プレーン (マスク等)
  - `1`: ブラシ。`(block_h*2, block_w)` として読み、2 面のうち 1 面のみ使う
  - `4`: 上記 RGBA

エンコード (書き戻し) 側は `encode_pixel_block` が逆変換を実装しており、
第 4 チャンネルはアルファで正規化される (= 元バイト完全一致にはならないが
意味のあるバイトは全再現)。

---

### 5.x 書く側 (逆写像) [実測]

`tools/clip_encode.py` が復号の逆をやる。往復が**完全一致**することを
合成画像と実ファイルのブロックの両方で確認済み。

```python
out = np.zeros((bh + 64, bw, 4), np.uint8)
out[64:, ..., 0:3] = rgba[..., [2, 1, 0]]        # B, G, R
a = rgba[..., 3].reshape(64, 4, 64, 4)           # (r, i, c, j)
for i in range(4):                               # alpha[y][x] -> [y//4][64*(y%4)+x//4][x%4]
    out[0:64, 64*i:64*(i+1), :] = a[:, i, :, :]
```

ブロックサブレコードの組み立てで踏みやすい点:

- `record_size` は **圧縮長 + 112**。空ブロックは **104 固定**
- 圧縮部の前にある `u32 BE` は「続くバイト数 = 圧縮長 + 4」。
  **その次の圧縮長だけがリトルエンディアン**
- 画素が 1 つも無いブロックは CSP も**空レコードを書く** (全面透明でも
  データありレコードを書いてはいけない、ということはないが、
  実ファイルに合わせるならこちら)
- 書き換えたら `Attribute` の `BlockSize[]` を差し替え、
  `ExternalChunk.Offset` を**全チャンク分計算し直す**

---

## 6. その他の BLOB 形式 (clip-tools 実績、未追検証)

| 対象 | 概要 |
|---|---|
| `ResizableImageInfo` / `Camera2DResizableImageInfo` | 120B ヘッダ (BE) + 4 隅の f64 座標。元サイズ→四隅へのホモグラフィで変形描画 |
| `TextLayerAttributes` | フォントスタイルブロック列 + チャンク列 + TLV 群。本文は `TextLayerString` (UTF-8) に別置き。未解決フィールド多数 |
| ベクタ blob (`VectorObjectList.VectorData`) | 88B ストロークヘッダ + 制御点 88B/点 (+CURVE 16B / BEZIER 32B)。`(88,72,88,88)`/`(88,72,104,88)`/`(88,72,120,88)` で種別判定 |
| `BrushStyle` (SQLite, ~69 列) | ブラシ形状・エフェクタ (筆圧カーブ)・スプレー・テクスチャ |
| `MonochromeFillInfo`, `LightTableInfo`, `FilterLayerInfo`, `CompLayerInfo` | 各種小 BLOB |

---

## 7. 未解決 / 要検証

### 解決済み (本調査で確定)

- ~~`Parameter` の `(33, 1, ?, 5)`~~ → 表現色と連動。4 通り確定 (§3.2.1)。
- ~~`has_color == 1` 時の追加 16 バイト~~ → 長さは `4 * num_extra` の可変長 (§3.2)。
- ~~`decompressed_size` は常に `(h+64)*w*4` か~~ → `(plane_count+1) * plane_bytes` (§3.2.1)。
- ~~`num_channels == 1` の 2 面の意味~~ → plane0 = α, plane1 = 値 (§3.2.1)。
- ~~サムネイルは「`MipmapInfo` に載らないもの」~~ → `LayerThumbnail` 経由で引く。
  さらに**どちらにも属さない第 3 のカテゴリが存在する** (§2.3)。

### 未解決

1. **16bit / CMYK**。`Canvas.CanvasChannelBytes` は全サンプル 1 で、CSP の新規作成
   ダイアログにも該当の選択肢が無い。CSP のキャンバス自体が 8bit のみと思われる。
   `color_mode` の 33 / 17 / 1 という数値の由来 (ビットフラグ?) も未確定。
2. `InitColor` の `magic = 20` がセクション種別かボディ長か区別できない。
3. `num_extra` 個の追加初期値は観測値が全てゼロ。着色した用紙で判明するはず。
4. **ベクタの描画**。ラスタが一切保存されないので `VectorObjectList.VectorData` から
   ストロークを起こす必要がある (ブラシエンジン)。テキストの配置は解決済み (§2.3)。
5. `BlockCheckSum` の算法。総当たりで外れたものは §4.2 に列挙。
   ゼロ/非ゼロが画素の有無と一致することだけ分かっている。
6. `LayerType = 8720` (nazoani01 に 2 レイヤ)。clip-tools の `LayerKind` にない値。
7. 調整レイヤ (`LayerType = 4098`) の効果の適用。`FilterLayerInfo` BLOB の解読が要る。
8. ~~合成モードは 8 種を実測同定済み~~ → **27 種すべて同定済み** (§9)。
9. binc チャンク (`Track` 50 件 in nazoani01) の中身の意味論。

---

## 8. 検証ログ

`tools/clip_lazy_demo.py` は **SQLite から計算したオフセットだけ**を使って
(バイナリ領域を走査せずに) 3 レイヤを取り出し合成する。結果:

```
sqlite region: offset=395129 len=724992
layer chain (first = bottom): [(4, '用紙'), (3, 'layer1'), (5, 'layer2')]
  layer 4 '用紙'   : offscreen=61 (700,1400,4) init_color=True
  layer 3 'layer1' : offscreen=67 (700,1400,4) init_color=False
  layer 5 'layer2' : offscreen=73 (700,1400,4) init_color=False

RGB diff vs test000.png: mean=0.0000 max=0 pixels>2: 0
```

同時に以下のアサーションが **4 ファイル合計 164,562 ブロックすべてで成立**した:

- `record_size == Attribute.BlockSize[i]`
- `section_size == compressed_size + 4`
- `len(zlib.decompress(...)) == decompressed_size`
- `8 + name_len*2 + 20 + 8 + clen + 4 + 34 == record_size`
- 空ブロックの `record_size == 104`
- ブロックの `(width, height)` が `Attribute` の値と一致
- `block_index` が 0 から連番
- ブロック列の後には `BlockStatus` / `BlockCheckSum` しか現れない
- `Attribute` のパース消費バイト数 == BLOB 長 (未消費の余りがない)

本番 3 ファイルでの追加確認:

```
                          tama      nazoani01   haruse
ExternalChunk 行数         124       1367        387
CHNKExta チャンク数        124       1367        387      ← 完全一致
Offset が CHNKExta を指す  124/124   1367/1367   387/387
SQLite 領域の占有率        11.5%     12.3%       5.6%
BlockSize[] は非空だが実体なし  332      700       1293  ← §2.1 の警告の根拠
```

### 8.1 CanvasPreview を正解とした合成の突き合わせ

`CanvasPreview.ImageData` は**キャンバスの完成画そのもの** (PNG)。等倍〜1/4 に縮む。
`clip_lazy_demo.py --preview` はこれを正解として比較する。

**28 ファイル中 13 がピクセル完全一致 (max=0)、22 が丸め誤差以内 (max<=2)。**

| 差分 | ファイル | 確定した仕様 |
|---|---|---|
| **max=0** | `test000` | RGBA 8bit のブロック展開 (参照 PNG) |
| **max=0** | `gray_empty` `gray_drawn` `mono_drawin` | 8bpp グレー / 1bpp モノクロ |
| **max=0** | `opacity` `mask` | 不透明度 0..256 / レイヤマスク |
| **max=0** | `folder` `passthrough` `emptyimage` | 分離フォルダ / 通過フォルダ |
| **max=0** | `text` `vector_resterized` | テキスト配置 / ラスタライズ済みベクタ |
| **max=0** | `adj_binarize` `adj_invert` | 調整レイヤ 2値化 / 階調の反転 |
| max=1 | `glow` `glow_folder` | **「発光」モードの α の扱い** (§9.1) |
| max=1 | `filter` `adj_tone1` `adh_tone2` | **トーンカーブ = ベジェ制御点** (§10.1) |
| max=1 | `adj_hue30` `adj_hue120` | 色相は度数で HSV 回転 |
| max=1 | `adj_bright` `adj_bright2` | 明るさは加算 |
| max=2 | `blendmodes` | 合成モード 8 種 |
| max=8 | `blend2` | 合成モード 19 種。残差は 彩度 のみ (§9) |
| max=21 | `adj_sat` | 彩度の式が未確定 (§10) |
| max=64 (168px) | `clipping` | クリッピング。縁 0.14% が残る |
| max=127 | `adj_val` | 明度の式が未確定 (§10) |
| 未対応 | `vector` | ラスタが存在しない。ブラシエンジンが要る |

## 9. 合成モードの実測同定

`samples/blendmodes.clip` / `samples/blend2.clip` は各モードのレイヤが別々の矩形を
塗るように作ってあるので、`CanvasPreview` 1 枚から複数モードを個別に同定できる。
**他のブレンドレイヤと重ならない画素だけ**を対象に候補式を総当たりした結果、
**27 種中 27 種を同定**した (`b` = 下地, `s` = 上のレイヤ, いずれも 0..1)。

| `LayerComposite` | CSP 名 | 式 | 最大誤差 |
|---|---|---|---|
| 0 | 通常 | `s` | 0 |
| 1 | 比較 (暗) | `min(b, s)` | 0 |
| 2 | 乗算 | `b * s` | 0 |
| 3 | 焼きこみカラー | `1 - min(1, (1-b)/s)` | 1 |
| 4 | 焼きこみ (リニア) | `b + s - 1` | 0 |
| 5 | 減算 | `b - s` | 0 |
| 6 | カラー比較 (暗) | 輝度の小さい方の**画素全体**を採る | 0 † |
| 7 | 比較 (明) | `max(b, s)` | 0 |
| 8 | スクリーン | `b + s - b*s` | 0 |
| 9 | 覆い焼きカラー | `min(1, b/(1-s))` | 1 |
| 10 | 覆い焼き (発光) | 同じ式だが **α の入れ方が違う** ‡ | 1 |
| 11 | 加算 | `b + s` | 0 |
| 12 | 加算 (発光) | 同じ式だが **α の入れ方が違う** ‡ | 0 |
| 13 | カラー比較 (明) | 輝度の大きい方の**画素全体**を採る | 0 † |
| 14 | オーバーレイ | `hard_light(s, b)` | 1 |
| 15 | ソフトライト | W3C soft-light | 0 |
| 16 | ハードライト | `hard_light(b, s)` | 1 |
| 17 | ビビッドライト | `s<=0.5 ? burn(b,2s) : dodge(b,2(s-0.5))` | 1 |
| 18 | リニアライト | `b + 2s - 1` | 0 |
| 19 | ピンライト | `s<=0.5 ? min(b,2s) : max(b,2s-1)` | 0 |
| 20 | ハードミックス | `b + s >= 1 ? 1 : 0` | 0 |
| 21 | 差の絶対値 | `abs(b - s)` | 0 |
| 22 | 除外 | `b + s - 2bs` | 0 |
| 23 | 色相 | `set_luma(set_sat(s, sat(b)), luma(b))` | 1 |
| 24 | 彩度 | `set_luma(set_sat(b, sat(s)), luma(b))` | **8** |
| 25 | カラー | `set_luma(s, luma(b))` | 2 |
| 26 | 輝度 | `set_luma(b, luma(s))` | 1 |
| 30 | 通過 | フォルダ専用。§9.1 | 0 |
| 36 | 除算 | `b / s` | 0 † |

`luma` の重みは **0.3 / 0.59 / 0.11** が最良 (Rec.709 や均等重みは明確に悪い)。[実測]

- † **この試験では一意に決まらなかった**もの。矩形の下にある下地がほぼ白 (0.894〜1.0)
  だったため式が飽和し、複数の候補が同点になった。名前と用途から確定させている
  (6/13 は `darken`/`lighten` とも同点、36 は `colordodge` 等とも同点)。
- ‡ **「発光」の正体は α の入れ方** [実測: `samples/glow.clip`]。§9.1 を見よ。
- 24 彩度だけ最大誤差 8 が残る (24,806 画素中 3,768 画素)。式の系統は正しい
  (他の候補は max 27〜230 と桁違いに悪い) ので、CSP 側の整数実装差と見ている。

### 9.1 「発光」モードの α の扱い [実測]

CSP には「覆い焼きカラー / 覆い焼き**(発光)**」「加算 / 加算**(発光)**」という
対のモードがある。**式そのものは同じで、違うのは α の入れ方**だった。

```
通常   : Co = (1-α)*Cb + α*B(Cb, Cs)          … α で補間する
発光   : Co = B(Cb, Cs * α)                    … s を α で乗じて直接ブレンド
```

`samples/glow.clip` (α グラデーション矩形 + レイヤ不透明度 50%) で検証:

| モード | 通常の式 | 発光の式 |
|---|---|---|
| 9 覆い焼きカラー | **max=1** | max=3 |
| 10 覆い焼き (発光) | max=3 | **max=1** |
| 11 加算 / 12 加算 (発光) | max=0 | max=0 |

**加算系は両者が代数的に一致する** (`(1-α)b + α(b+s) = b + αs = b + (s·α)`) ので、
11 と 12 は結果で区別できない。非線形な覆い焼きでだけ差が出る。

α には**画素のアルファとレイヤ不透明度の両方**が入る (どちらでも同じ傾向が出た)。

下地が透明な場合にも成立させるには、一般形を次のようにする:

```
αo    = αs + αb*(1-αs)
Co*αo = (1-αb)*αs*Cs + αb*B(Cb, Cs*αs)
```

αb=1 で `B(Cb, Cs*αs)`、αb=0 で通常合成に帰着する。
`samples/glow_folder.clip` (分離フォルダ内で下地が透明) で確認済み。

### 9.2 フォルダの合成 (通過とそれ以外) [実測]

- **通過 (`LayerComposite == 30`)**: 分離しない。子は**フォルダの外の下地**へ直接
  合成される。中のレイヤの合成モードがフォルダ外にも効く。
- **それ以外**: 透明なバッファに子を合成してから、フォルダ自身の合成モード /
  不透明度 / マスクで親へ重ねる。中の合成はフォルダ内で閉じる。

このため合成器は**下地が透明な場合にも正しい一般形**が要る:

```
αo    = αs + αb*(1-αs)
Co*αo = (1-αb)*αs*Cs + αb*αs*B(Cb,Cs) + (1-αs)*αb*Cb
```

下地が不透明 (αb=1) なら `(1-αs)*Cb + αs*B` に帰着し、透明 (αb=0) なら
合成モードによらず通常合成になる (分離フォルダの最下段レイヤがこれに当たる)。
`samples/passthrough.clip` で両方の経路がピクセル完全一致することを確認済み。

### 9.3 その他の実測

- `LayerOpacity` は **0..256**。α への適用は `α * opa // 256` (切り捨て) が一致。
- レイヤマスクは `α * mask // 255` (切り捨て) が一致 (丸めると max=1 ずれる)。
- クリッピング (`LayerClip != 0`) は直下の非クリップレイヤの α で絞ると
  120,000 画素中 168 画素を除いて一致する。グループとして合成するモデルも試したが
  残差はほぼ同じで、縁の扱いに CSP 固有の差がある模様。

---

## 10. 調整レイヤ (`FilterLayerInfo`) [実測]

`LayerType & 4096` が調整レイヤ。画素を持たず、**自分より下の合成結果を書き換える**。
効果は `Layer.FilterLayerInfo` BLOB に入っており、構造は極めて素直だった:

```
u32 BE  filter_type
u32 BE  payload_bytes
i32 BE  params[payload_bytes / 4]
```

`filter_type` は clip-tools の `FilterLayerKind` と一致する:
1=明るさ・コントラスト, 2=レベル補正, 3=トーンカーブ, 4=色相・彩度・明度,
5=カラーバランス, 6=階調の反転, 7=ポスタリゼーション, 8=2値化,
9=グラデーションマップ。

実測したもの:

| 種別 | CSP 名 | パラメータ | 式 | 誤差 |
|---|---|---|---|---|
| 1 | 明るさ・コントラスト | `[明るさ, コントラスト]` | `v + 明るさ` | 1 |
| 3 | トーンカーブ | 下記 | ベジェ曲線の LUT | 1 |
| 4 | 色相・彩度・明度 | `[色相, 彩度, 明度]` | HSV の色相を**度**で回す | 1 |
| 6 | 階調の反転 | (なし。`payload_bytes = 0`) | `255 - v` | 0 |
| 8 | 2値化 | `[しきい値]` | **チャンネルごとに** `v >= th ? 255 : 0` | 0 |

- **2値化は輝度ではなくチャンネルごと** [実測: 輝度でやると max=255]。
- **色相の単位は度** [実測: +30 と +120 の 2 ファイルで確認]。
- **彩度 / 明度の式は未確定**。`adj_sat.clip` (彩度+50) は無変換で max=21、
  HSL の `S + (1-S)*0.5` が最良で max=11。`adj_val.clip` (明度+50) は
  加算・HSV V・HSL L のいずれも max≥77 で当たらない。**適用しない方がマシ**な水準
  なので、現状ツールでは色相のみ適用している。
- コントラスト / レベル補正 (2) / カラーバランス (5) / ポスタリゼーション (7) /
  グラデーションマップ (9) は未測定。**ポスタリゼーションは CSP 5.0.4 の
  メニューに項目が無い**との報告あり (旧バージョン専用か、名称が違う)。

### 10.1 トーンカーブ (種別 3) [実測]

payload は **`u16 BE count + 32 x (u16 BE x, u16 BE y)` のブロックが 32 本**
(1 ブロック = 65 u16 = 130 バイト、合計 4160 バイト)。先頭ブロックが合成チャンネル、
以降が R / G / B。座標は **0..65535** で、`v/65535*255` が 0..255 の値になる。

**格納された点は「曲線が通る点」ではなく、ベジェの制御点** [実測]。

```
adj_tone1.clip : count=2, [(0, 16384), (65535, 65535)]        → 出力下端 63.75
adh_tone2.clip : count=2, [(0, 32530), (65535, 65535)]        → 出力下端 126.6
filter.clip    : count=3, [(0,0), (11703,55562), (65535,65535)]
```

`filter.clip` の 3 点を **2 次ベジェの制御点**として曲線を引き、x で並べ替えて
LUT にすると **max=1 で一致**する。線形補間では max=96、PCHIP で 99、
自然スプラインで 119 と、いずれも合わない。

2 点の場合は次数 1 = 直線なので線形補間と同じ結果になる
(`adj_tone1` / `adh_tone2` がそれで max=1)。

4 点以上のときに単一の高次ベジェなのか区分ベジェなのかは**未確認**。
