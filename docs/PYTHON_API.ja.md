# clipparse Python API リファレンス

[English version](PYTHON_API.md) — このページの英語版。

`clipparse` は **Python の依存パッケージを持たない** C++ 拡張モジュール 1 つ。
CLIP STUDIO PAINT の `.clip` を遅延読み込みし (画素は 256x256 ブロック単位で、
必要になった時にだけ展開する)、合成し、CSP が受け付ける形で書き戻す。

```bash
pip install clipparse
```

```python
import clipparse
```

公開しているのは次の 5 つ。

| | |
|---|---|
| [`ClipFile`](#class-clipparseclipfile) | 読む側。メタ情報・レイヤツリー・画素・合成 |
| [`LayerInfo`](#class-clipparselayerinfo) | レイヤ 1 枚の読み取りビュー (直接は作らない) |
| [`OffscreenAttr`](#class-clipparseoffscreenattr) | `Offscreen.Attribute` を解いたもの。寸法とブロック長 |
| [`ClipWriter`](#class-clipparseclipwriter) | 書く側。編集して保存する |
| [`validate()`](#clipparsevalidatepath) | 書いたファイルを CSP に見せる前に検査する |

## 最初に押さえること

**画素は BGRA の `bytes`、ストレートアルファ。** このモジュールが返す画像はすべて
長さ `width * height * 4` の `bytes` で、B, G, R, A の順。psdparse と同じ約束にしてある。
[画素の形式](#画素の形式)を参照。

**読む側は添字、書く側は `main_id`。** `ClipFile` はレイヤを `f.layers` の位置で
指す。`ClipWriter` は CLIP 自身の `Layer.MainId` で指す。相互変換は
`f.layers[i].main_id`。

**レイヤのリストは平坦・下から上・中身がフォルダより先。** psdparse とまったく同じ
順序なので、片方向けに書いたコードがもう片方でも動く。ツリーは派生ビューで、
`f.roots` と `f.children(i)` で辿る。

**不透明度には 2 つの尺度がある。** CLIP の格納値は 0..**256**。`layer.opacity` は
PSD に合わせて 0..255 に直したもの、`layer.opacity_raw` が CLIP の生値で、
`ClipWriter.set_layer_attr(opacity=...)` は**生値の 0..256** を取る。

**パスは `str`。** 内部で UTF-8 に符号化する (Windows では mmap の前に UTF-16 へ変換)。

**1 つのオブジェクトは 1 スレッドで。** `ClipFile` は解いた属性を内部にキャッシュする
ので、スレッド安全ではない。スレッドごとにインスタンスを持つこと。呼び出しは GIL を
握ったままなので、そもそもスレッドでは重ならない — 並列化したければプロセスを使う。

**`ClipFile` を生かしておくこと。** `LayerInfo` はそれを作ったファイルへのビューで、
`ClipFile` が回収された後も持ち続けると未定義動作になる。

---

## `class clipparse.ClipFile`

```python
f = clipparse.ClipFile()
```

### 読み込み

```python
f.load(path: str) -> bool
```

ファイルを mmap して解析する。**触るのは埋め込みの SQLite (メタ情報) だけ**なので、
90 MB の作品でも数十〜数百ミリ秒で終わる (画素ブロックには一切触らない)。
失敗しても例外ではなく `False` を返し、理由は `f.error` に入る。

```python
f.is_loaded   # bool  — レイヤが 1 枚以上ある状態で読めていれば True
f.error       # str   — 直近の失敗理由。例: 'cannot open/map file'
```

### キャンバス

```python
f.width        # int   — キャンバス幅 (ピクセル)
f.height       # int   — キャンバス高 (ピクセル)
f.resolution   # float — 解像度 (DPI)
```

ファイル内の `Canvas.CanvasWidth` は `CanvasUnit` の単位で、**ミリメートルの実ファイル
が存在する**ため使えない。この 2 つはルートフォルダの 100% ミップから導いてあり、
常にピクセル値になる。

### レイヤ

```python
f.layers       # list[LayerInfo] — 平坦・下から上・中身がフォルダより先
f.roots        # list[int]       — 最上位レイヤの添字。下から上
f.children(i)  # list[int]       — layers[i] の直接の子。-1 で最上位
```

`children()` は範囲外の添字で `IndexError`。

```python
def walk(f, index=-1, depth=0):
    for i in f.children(index):
        layer = f.layers[i]
        print("  " * depth + layer.name)
        if layer.is_group:
            walk(f, i, depth + 1)
```

### 画素

```python
f.layer_image(index: int, mode: str = "masked") -> bytes
```

レイヤ 1 枚の画素を BGRA バイト列で返す。長さは
`layer.width * layer.height * 4` — つまり**キャンバスではなくレイヤの外接矩形**
(`layer.left/top/width/height`) の大きさ。実体のあるブロックしか展開しない。

- `mode="masked"` (既定) — マスクをアルファに繰り込んだ画像
- `mode="image"` — マスクを無視した画像
- `mode="mask"` — マスクだけをグレースケールとして BGRA に入れたもの

自前の画素を持たないレイヤ (フォルダ) では `b""` が返る。添字が範囲外なら
`IndexError`、`mode` が不正なら `ValueError`。

```python
f.layer_region(index: int, x: int, y: int, width: int, height: int,
               mode: str = "masked") -> bytes
```

同じものを矩形 `(x, y, width, height)` **(キャンバス座標)** の分だけ読む。
**重なる 256x256 ブロックしか展開しない** — 行 RLE の PSD には真似のできない、
CLIP 固有の利点。戻り値は常に `width * height * 4` バイト
(キャンバスの外は透明で埋まる)。空の矩形では `b""`。

```python
f.merged_image() -> bytes
```

表示状態のレイヤを下から合成して `width * height * 4` の BGRA バイト列を返す。
合成モード・フォルダの入れ子 (通過を含む)・マスク・クリッピング・調整レイヤを
すべて適用する。**CSP 自身がファイルに保存した絵を再現するのが目標**で、
サンプル 28 本のうち 13 本が画素完全一致、22 本が丸め誤差以内。

```python
f.preview_png() -> tuple[bytes, int, int] | None
```

CSP がファイルに保存しているプレビュー画像 (`CanvasPreview`) を
`(png_bytes, width, height)` で返す。無ければ `None`。CSP がレンダリングした
完成画そのものなので**正解合わせに使える**が、等倍とは限らない。

### 低水準の口

普段は要らない。上の各プロパティが乗っている格納層を直接見るためのもの。

```python
f.top_offscreen(layer_main_id: int, mask: bool = False) -> int
```

レイヤの画像 (`mask=True` ならマスク) の 100% ミップ段の `Offscreen.MainId`。
無ければ `0`。

```python
f.attribute(offscreen_id: int) -> OffscreenAttr | None
```

その offscreen の `Offscreen.Attribute` を解いたもの。未知の ID なら `None`。

```python
f.check() -> tuple[bool, str]
```

全ブロックを走査して構造の不変条件 (ブロック長・オフセット・プレーン構成) を
検査する。`(ok, report)` を返し、`report` は人間が読む要約。これは**読み**の
整合性を見るもので、**書いた**ファイルには
[`validate()`](#clipparsevalidatepath) を使う。

---

## `class clipparse.LayerInfo`

`ClipFile.layers` から得られる、レイヤ 1 枚の読み取り専用ビュー。両形式に概念が
ある属性は psdparse の `LayerInfo` と名前を揃えてある。

| 属性 | 型 | 備考 |
|---|---|---|
| `index` | `int` | `ClipFile.layers` 内の位置。**読む側**が取る値 |
| `main_id` | `int` | CLIP の `Layer.MainId`。**書く側**が取る値 |
| `layer_id` | `int` | `main_id` と同じ値 (psdparse 互換) |
| `name` | `str` | レイヤ名 |
| `name_unicode` | `str` | `name` と同じ。CLIP は名前を UTF-8 で持つので、PSD のような生名/Unicode 名の対は存在しない |
| `visible` | `bool` | |
| `opacity` | `int` | 0..255。CLIP の 0..256 から換算したもの |
| `opacity_raw` | `int` | CLIP の生の `LayerOpacity`。0..**256** |
| `fill_opacity` | `int` | 常に 255 (CLIP に塗り不透明度は無い) |
| `clipping` | `int` | 下のレイヤでクリッピングしていれば 1 |
| `composite_raw` | `int` | 生の `LayerComposite`。[合成モード](#合成モード)を参照 |
| `is_group` | `bool` | レイヤフォルダ |
| `is_filter` | `bool` | 調整レイヤ (自前の画素を持たない) |
| `is_text` | `bool` | テキストレイヤ |
| `has_mask` | `bool` | |
| `transparency_protected` | `bool` | 常に `False` (未対応) |
| `left`, `top`, `right`, `bottom` | `int` | キャンバス上の外接矩形 |
| `width`, `height` | `int` | 外接矩形から導く。フォルダは `0` |
| `parent_index` | `int` | 属するフォルダの添字。最上位なら `-1` |
| `children` | `list[int]` | 直接の子の添字。下から上。フォルダ以外は空 |

外接矩形は画素が存在する範囲。通常のラスタレイヤではキャンバス全面になることが
多く、テキストレイヤでは文字の外接矩形になる。

---

## `class clipparse.OffscreenAttr`

`Offscreen.Attribute` の BLOB を解いたもの — 1 枚のラスタがファイル内でどう
並んでいるか。ブロックを直に扱うときだけ必要になる。寸法・表現色に加え、
**前置和を取れば任意ブロックの位置が決まる**ブロック長の配列を持つ。

| 属性 | 型 | 備考 |
|---|---|---|
| `width`, `height` | `int` | このラスタの論理サイズ |
| `cols`, `rows` | `int` | ブロックグリッド |
| `block_width`, `block_height` | `int` | ほぼ常に 256 x 256 |
| `color_mode` | `int` | 33 = RGBA, 17 = グレー/モノクロ, 1 = マスク |
| `num_channels` | `int` | 4 / 1 / 0 |
| `bit_depth` | `int` | 5 = 8bpp RGBA, 2 = 8bpp, 1 = 1bpp |
| `plane_bytes` | `int` | プレーンあたりのバイト数 |
| `has_init_color` | `bool` | 初期色を持つか |
| `init_color` | `int` | その色。RGBA をビッグエンディアンに詰めたもの |
| `block_sizes` | `list[int]` | ブロックごとの**サブレコード全長** (圧縮サイズではない)。104 = 空ブロック、それ以外は圧縮長 + 112 |

---

## `class clipparse.ClipWriter`

```python
w = clipparse.ClipWriter()
```

書く側はファイル全体をメモリに読む (**読み込んだのと同じパスへ書き戻せる**)。
編集は埋め込みの SQLite とチャンクの一覧に対して行い、`save()` で全チャンクの
オフセットを計算し直す。

**無変更の往復はバイト一致する:**

```python
w.load("in.clip"); w.save("out.clip")     # sha256(out) == sha256(in)
```

以下のメソッドは失敗するとライブラリのメッセージ付きで `RuntimeError` を送出する。
渡した幅・高さと画素バッファの大きさが合わない場合は `ValueError`。

### 読み込みと保存

```python
w.load(path: str) -> bool                 # 失敗すると RuntimeError
w.save(path: str) -> int                  # 書いたバイト数を返す
```

### `set_layer_attr` — レイヤ属性

```python
w.set_layer_attr(main_id: int, name: str | None = None, opacity: int = -1,
                 visible: int = -1, composite: int = -1, clipping: int = -1,
                 folder: int = -1) -> bool
```

レイヤ 1 枚の属性を変える。`-1` (名前は `None`) はその項目を変更しない。
触るのは SQLite だけなので、**チャンクは 1 つも動かず、ファイルサイズも変わらない**。

- `opacity` — **0..256** (CLIP の尺度)
- `visible` — 0 / 1
- `composite` — [合成モード](#合成モード)を参照
- `clipping` — 0 / 1
- `folder` — bit0 = フォルダ, bit4 = 折り畳み

### `set_pixels` — 画素の差し替え

```python
w.set_pixels(main_id: int, bgra: bytes, width: int, height: int) -> bool
```

レイヤの 100% ミップを差し替える。**バッファはキャンバス全面**でなければならない
— `width` / `height` が 100% ミップと違うと
`RuntimeError: pixel size does not match the 100% mipmap` になる。アルファ面の
畳み込み・再圧縮・`BlockSize[]` の書き戻し・以降の `ExternalChunk.Offset` の
再計算まで行う。

古くなったサムネイルは**自動で落とす** (CSP が作り直す)。ただし `CanvasPreview` は
更新しないので、[レシピ](#レイヤの画素を差し替える)を参照。

### `add_layer` / `delete_layer`

```python
w.add_layer(copy_from: int, name: str, bgra: bytes | None = None,
            width: int = 0, height: int = 0,
            after: int = -1, parent: int = 0) -> int
```

**既存レイヤを雛形として丸ごと複製し**、ID・リンク・画素だけ差し替えてレイヤを
足す (`Layer` は 57 列あり、CSP が期待する既定値の大半は意味が分からない。実在の
行を写せば当てずっぽうを書かずに済む)。新しい `Layer.MainId` を返す。

- `copy_from` — 雛形にするレイヤの `main_id`
- `bgra` — キャンバス全面の画素。`None` なら全面透明のレイヤ
- `after` — この `main_id` の 1 つ上に挿す。`-1` で最上段
- `parent` — 入れるフォルダの `main_id`。`0` でキャンバスのルート

```python
w.delete_layer(main_id: int) -> bool
```

レイヤ本体・ミップ連鎖・サムネイルと、それらのチャンクを消し、兄弟の連結を
繋ぎ直す。`Canvas.CanvasCurrentLayer` が消したレイヤを指していた場合は
別のレイヤへ向け直す。

### `set_canvas_preview`

```python
w.set_canvas_preview(bgra: bytes, width: int, height: int) -> bool
```

**CSP がファイルを開いた瞬間に表示する画像**を差し替える。古いままだと、ユーザーが
レイヤを触るまで違う絵が出るので、画素を編集したら必ず作り直すこと。

### `resize_canvas`

```python
w.resize_canvas(width: int, height: int, dpi: float = 0.0) -> bool
```

キャンバスの寸法ごと作り替える。ミップ連鎖の段数も、その寸法で CSP が作る段数へ
伸縮させる。**実体は全部落ちる**ので、後から `set_pixels()` で入れ直すこと。
`tools/psd_to_clip.py` が空の雛形を任意寸法のキャンバスに変えるのに使っている。

### `set_external_id_seed`

```python
w.set_external_id_seed(seed: int) -> None
```

新しい外部チャンク ID の乱数種を固定する。テストで再現性が要るとき用。

---

## `clipparse.validate(path)`

```python
clipparse.validate(path: str) -> list[str]
```

ディスク上の `.clip` の参照整合性を検査し、見つかった問題を返す。
**空のリストなら健全**。ミップ段数と連鎖の食い違い、閉路、孤児行、テーブルごとの
格納型の誤り、消えたレイヤへの参照を見る。

**書いたファイルを CSP で開く前に必ず通すこと。** ここで捕まる種類の失敗は、
まさに**寛容なリーダでは検出できない**もの — こちらのライブラリでは問題なく
読み戻せるのに、CSP ではレイヤが全面透明になったり、「レイヤ画像が破損しています」
と言われたり、読み込み中に落ちたりする。

```python
problems = clipparse.validate("out.clip")
if problems:
    raise SystemExit("\n".join(problems))
```

---

## CSP が強制してくる作法

CLIP STUDIO PAINT PRO 5.0.4 の実機で 5 巡かけて洗い出したもの。`ClipWriter` が
行う編集についてはすべて面倒を見ているので、この一覧が要るのは**その外側**
(`tools/` から生の SQLite を触る、自前の書き出しを作る) で作業するとき。

1. `Offscreen.BlockData` は **BLOB**、`ExternalChunk.ExternalID` は **TEXT**。
   同じ 40 文字の ID なのに格納型が逆。前者を間違えるとそのレイヤは全面透明で開き、
   後者を間違えると `UPDATE` が 1 行もマッチせず黙って失敗する。
2. `BlockCheckSum` は **0** にする。CSP は非ゼロのチェックサムを実際に照合しており、
   算法は未特定。0 は「検査値なし」扱いで通る。
3. `Mipmap.MipmapCount` は**必ず連鎖の段数と一致**させる。違うと CSP が
   **読み込み中に落ちる**。
4. 画素を書き換えたら、サムネイルの実体を落とすだけでなく
   `LayerThumbnail.Thumbnail*NeedRefresh` に **50** を入れる。この列は世代番号で、
   実体を消しただけでは古いサムネイルが残る。
5. `CanvasPreview` を合成し直す。開いた瞬間にユーザーが見るのはこれ。
6. `Canvas.CanvasCurrentLayer` は**生きているレイヤ**を指していること。

---

## 合成モード

`layer.composite_raw` と `ClipWriter.set_layer_attr(composite=...)` は CLIP 自身の
番号を使う。27 種すべてを CSP の合成結果と突き合わせて実測同定してある。式は
[CLIP_FORMAT.md §9](CLIP_FORMAT.md) にある。

| | | | | | |
|---|---|---|---|---|---|
| 0 | 通常 | 11 | 加算 | 21 | 差の絶対値 |
| 1 | 比較 (暗) | 12 | 加算 (発光) | 22 | 除外 |
| 2 | 乗算 | 13 | カラー比較 (明) | 23 | 色相 |
| 3 | 焼きこみカラー | 14 | オーバーレイ | 24 | 彩度 |
| 4 | 焼きこみ (リニア) | 15 | ソフトライト | 25 | カラー |
| 5 | 減算 | 16 | ハードライト | 26 | 輝度 |
| 6 | カラー比較 (暗) | 17 | ビビッドライト | 30 | 通過 (フォルダ専用) |
| 7 | 比較 (明) | 18 | リニアライト | 36 | 除算 |
| 8 | スクリーン | 19 | ピンライト | | |
| 9 | 覆い焼きカラー | 20 | ハードミックス | | |
| 10 | 覆い焼き (発光) | | | | |

「(発光)」の 2 種は通常版と式は同じで、**アルファの入れ方だけが違う**。

---

## 画素の形式

画像はすべて `bytes`。1 画素 4 バイトの **B, G, R, A**、ストレートアルファ、
行は上から下、パディングなし。

NumPy:

```python
import numpy as np
a = np.frombuffer(f.merged_image(), np.uint8).reshape(f.height, f.width, 4)
rgba = a[..., [2, 1, 0, 3]]        # BGRA -> RGBA
```

Pillow:

```python
from PIL import Image
img = Image.frombytes("RGBA", (f.width, f.height), f.merged_image())
b, g, r, a = img.split()
img = Image.merge("RGBA", (r, g, b, a))
```

逆向き (Pillow / NumPy から `ClipWriter` へ) はチャンネルを戻して渡す。
バッファプロトコルを持つオブジェクトなら何でもよく、C 連続で大きさが合っていれば
NumPy 配列をそのまま渡せる。

---

## エラーの扱い

| 状況 | 挙動 |
|---|---|
| `ClipFile.load()` がファイルを開けない・壊れている | `False` を返し、理由は `f.error` |
| レイヤの添字が範囲外 | `IndexError` |
| `mode` の文字列が不正 | `ValueError` |
| 画素バッファの長さが `width * height * 4` と違う | `ValueError` |
| `ClipWriter` の操作が失敗 (レイヤが無い・画素サイズ違い・保存失敗) | ライブラリのメッセージを載せた `RuntimeError` |
| `ClipFile.attribute()` に未知の ID | `None` を返す |
| `ClipFile.preview_png()` でプレビューが無い | `None` を返す |

---

## レシピ

### 全レイヤを PNG に書き出す

```python
import clipparse
from PIL import Image

f = clipparse.ClipFile()
f.load("artwork.clip")

for layer in f.layers:
    if layer.is_group or not layer.width:
        continue
    data = f.layer_image(layer.index)
    img = Image.frombytes("RGBA", (layer.width, layer.height), data)
    b, g, r, a = img.split()
    Image.merge("RGBA", (r, g, b, a)).save(f"layer{layer.index:02d}.png")
```

### 巨大なキャンバスの一部だけ読む

```python
f = clipparse.ClipFile()
f.load("big.clip")                       # メタ情報だけ。まだ何も展開していない
tile = f.layer_region(7, 4096, 2048, 512, 512)   # 重なるブロックだけ展開
```

遅延方式の狙いはここにある。**コストはファイルの大きさではなく、要求した領域に
比例する。**

### レイヤ属性を変える

```python
w = clipparse.ClipWriter()
w.load("in.clip")
w.set_layer_attr(main_id, name="線画", opacity=192, composite=2)  # 乗算
w.save("out.clip")
assert clipparse.validate("out.clip") == []
```

属性だけの編集は SQLite しか書き換えないので、大きなファイルでも安い。

### レイヤの画素を差し替える

`set_pixels()` は古いサムネイルを落としてくれるが、`CanvasPreview` は
**書き上がったファイルから合成し直す**必要がある。いったん保存し、読み直して
合成し、プレビューを書き戻す:

```python
w = clipparse.ClipWriter()
w.load("in.clip")
w.set_pixels(main_id, bgra, canvas_w, canvas_h)
w.save("out.clip")

f = clipparse.ClipFile()                 # 書き上がったファイルを開き直す
f.load("out.clip")
merged, cw, ch = f.merged_image(), f.width, f.height
del f                                    # 書く前に mmap を手放す

w2 = clipparse.ClipWriter()
w2.load("out.clip")
w2.set_canvas_preview(merged, cw, ch)
w2.save("out.clip")

assert clipparse.validate("out.clip") == []
```

### レイヤを足す

```python
w = clipparse.ClipWriter()
w.load("in.clip")
new_id = w.add_layer(template_main_id, "追加", bgra, canvas_w, canvas_h,
                     after=below_main_id, parent=folder_main_id)
w.save("out.clip")
```

`bgra=None` なら空のレイヤ。CSP で開いてそのまま描き込み、上書き保存できる。

---

## 対象外

- **ベクタレイヤ。** ラスタが一切保存されていないため、再現にはブラシエンジンが要る。
  CSP でラスタライズしてから読めば完全に一致する。
- **一部の調整レイヤ。** 明るさ・トーンカーブ・色相・階調の反転・2 値化は実装済み。
  レベル補正・カラーバランス・ポスタリゼーション・グラデーションマップは未対応。
- **テキストをテキストとして読むこと。** テキストレイヤはレンダリング済みの
  ラスタとして読める。文字列・フォント・組みは公開していない。
- **PSD 変換。** psdparse が要るのでホイールには入っていない。リポジトリの
  `tools/clip_to_psd.py` / `tools/psd_to_clip.py`、または C++ の
  `examples/clipconv` を使う。

## リポジトリ側にあるもの

`tools/imgdoc.py` は `.clip` に **psdparse 互換の読み取り面**を被せる。psdparse
向けに書いたスクリプトを 1 行も直さずに CLIP ファイルへ向けられる。

```python
import imgdoc
doc = imgdoc.open("file.clip")      # .psd なら psdparse.PSDFile をそのまま返す
doc.header.width, doc.header.height
for i in doc.roots:
    print(doc.layers[i].name_unicode, doc.layers[i].children)
doc.layer_image(0)                  # BGRA bytes
```

`layer_type` / `blend_mode` は psdparse の enum をそのまま返すので、
`layer.blend_mode == psdparse.BlendMode.MULTIPLY` のような比較が通る。C++ 拡張が
あればそれを、無ければ純 Python の参照実装を使う (どちらかは `imgdoc.BACKEND`)。
psdparse を要求する = 依存ゼロでなくなるため**ホイールには同梱していない**。
リポジトリから取ってくること。
