# 作業状況と再開手順

最終更新: 2026-08-17 / フェーズ **書き込みが CSP 実機で全項目 OK。C++ 側にも移植済み**

---

## 再開したらまず動かすもの

```powershell

# 回帰: 参照 PNG とのピクセル一致
python tools/clip_lazy_demo.py samples/test000.clip --compare samples/test000.png

# 回帰: 各ファイル内蔵の CanvasPreview との一致 (max<=2 なら exit 0)
python tools/clip_lazy_demo.py samples/gray_drawn.clip  --preview
python tools/clip_lazy_demo.py samples/mono_drawin.clip --preview
python tools/clip_lazy_demo.py samples/opacity.clip     --preview
python tools/clip_lazy_demo.py samples/mask.clip        --preview
python tools/clip_lazy_demo.py samples/blendmodes.clip  --preview

# 構造アサーション (エラーが出なければ健全)
python tools/clip_probe.py samples/test000.clip --blocks
```

### 現在の一致状況 (28 ファイル中 13 が完全一致、22 が丸め誤差以内)

| 差分 | ファイル |
|---|---|
| **max=0** | `test000` `gray_empty` `gray_drawn` `mono_drawin` `opacity` `mask` `folder` `passthrough` `emptyimage` `text` `vector_resterized` `adj_binarize` `adj_invert` |
| max=1〜2 | `glow` `glow_folder` `filter` `adj_tone1` `adh_tone2` `adj_hue30` `adj_hue120` `adj_bright` `adj_bright2` `blendmodes` |
| max=8 | `blend2` (残差は 彩度 のみ) |
| max=21 / 127 | `adj_sat` / `adj_val` (彩度・明度の式が未確定) |
| max=64 (168px) | `clipping` (縁 0.14%) |
| 未対応 | `vector` (ブラシエンジンが要る) |

実装済み: 表現色 4 種 / 不透明度 / レイヤマスク / クリッピング / **合成モード 27 種** /
**「発光」モードの α 扱い** / テキスト配置 / 通過・分離フォルダ /
**調整レイヤ 5 種 (明るさ・トーンカーブ・色相・階調の反転・2値化)**。

必要なもの: Python 3.12 / numpy / Pillow (導入済み)。`clip_probe.py` は標準ライブラリのみ。
**pandas と cv2 は未導入**(clip-tools 本体を動かすときだけ必要。§参考 を見よ)。

---

## いまどこまで分かっているか

- `.clip` の形式は解析済み → [CLIP_FORMAT.md](CLIP_FORMAT.md)
- psdparse 型の遅延参照が成立することを実証済み → [DESIGN.md](DESIGN.md)
- 検証規模: `test000` + 本番 3 ファイル、**164,562 ブロック**で構造アサーション全通過
- `test000.clip` は参照 PNG と **ピクセル完全一致 (max diff = 0)**
- clip-tools のバグ 2 件 + 仕様訂正 2 件を再現込みで文書化 → [CLIP_TOOLS_REPORT.md](CLIP_TOOLS_REPORT.md)
  (**まだ先方に送っていない**。送るタイミングは要判断)

### 特に忘れやすい要点

1. `ExternalChunk.Offset` + `Attribute.BlockSize[]` の累積和で**任意ブロックへ直接シーク**できる
2. `BlockSize[i]` は**サブレコード全長**。空ブロック = 104、データあり = 圧縮長 + 112
3. **ミップ連鎖は `Layer.LayerRenderMipmap` → `Mipmap.BaseMipmapInfo` → `MipmapInfo.NextIndex`**。
   `MipmapInfo` を `(LayerId, ThisScale=100)` で引くとマスク用連鎖と衝突する
4. `InitColor` の末尾は**可変長** (`4 * quad[2]` バイト)。16 バイト固定で読むと壊れる
5. `Canvas.CanvasWidth/Height` は `CanvasUnit` の単位で、**ピクセルとは限らない**。
   実ピクセル寸法はルートフォルダの 100% ミップの `Attribute` から取る
6. **縮小ミップ段の画素は存在しない** (本番 3 ファイルで 0/69・0/208・0/452)。
   100% 段とサムネイルにしか画素は書かれない
7. `MainId` の採番元は `ElemScheme.MaxIndex` (SQLite の AUTOINCREMENT ではない)
8. 表現色でプレーン構成が変わる。**グレー/モノクロは plane0 = α, plane1 = 値**
   (順序が直感と逆)。モノクロの 100% 段だけ 1bpp で、縮小段は 8bpp グレー
9. Offscreen には**ミップ段でもサムネイルでもない第 3 のカテゴリ**がある。
   テキスト/ベクタの外接矩形ラスタで、FK が無く `Offscreen.LayerId` でしか辿れない

---

## 次にやること (優先順)

### ① CSP 実機でサンプル作成

- **第 1 弾 完了** — [SAMPLE_REQUESTS.md](SAMPLE_REQUESTS.md) の A〜C が `samples/` に
  揃った (`mono_empty` のみ無し。描画済みがあるので支障なし)。
  これにより表現色・プレーン構成・合成モード 8 種が確定した。
- **第 2 弾 完了** — [SAMPLE_REQUESTS_2.md](SAMPLE_REQUESTS_2.md)。
  `blend2.clip` (19 種を 1 ファイルに集約) / `passthrough` / `adj_*` 4 種 /
  `vector_resterized`。`adj_posterize` のみ未作成。
- **第 3 弾 完了** — [SAMPLE_REQUESTS_3.md](SAMPLE_REQUESTS_3.md)。
  `glow` `glow_folder` `adj_hue30/120` `adj_sat` `adj_val` `adj_invert`
  `adj_tone1` `adh_tone2`。ポスタリゼーションは **CSP 5.0.4 のメニューに項目が無い**
  との報告で作成なし。

サンプルを書き出した CSP は **PRO 5.0.4** (`samples/versions.txt`)。
公式サンプル 3 本だけ 2022 年頃の旧バージョンで、`Layer` の列数が
52 / 106 / 113 と新しいファイル (56〜57 列) と食い違う。バージョン差の検証に使える。

### ② Python で合成を `CanvasPreview` に合わせ込む ← **いまここ**

第 2 弾サンプルで合成モード 27 種・通過フォルダ・調整レイヤの構造が確定した。
残っているもの (優先順):

第 3 弾で「発光」の正体・トーンカーブ・色相が確定した。**合成の詰めはほぼ完了**。
残っているのは以下だけで、いずれも本流ではない:

1. **色相・彩度・明度の彩度 (p[1]) と明度 (p[2]) の式**。適用しない方がマシな水準
   なので現状は色相のみ適用。彩度は HSL の `S+(1-S)*v/100` が最良で max=11。
2. **合成モード 24 彩度の残差 8**。式の系統は正しいので CSP の整数実装差と見ている。
3. **クリッピングの残り 168 画素**。縁の扱い。
4. **調整レイヤの未測定種別** (レベル補正 2 / カラーバランス 5 /
   ポスタリゼーション 7 / グラデーションマップ 9)。構造は解読済みなので式だけ。
5. **ベクタ**は当面やらない。ラスタが一切保存されておらずブラシエンジンが要る。
   `vector_resterized.clip` が完全一致したので「CSP でラスタライズしてもらう」
   実用経路は裏付けが取れている。

**③④ に着手済み (下記)。**

**C++ 移植より先にこれをやる。** 合成を書くと「レイヤに何を問い合わせる必要があるか」
(マスク / クリッピング / パススルー / フォルダ不透明度 / 調整レイヤ) が確定し、
それが決まらないと C++ の API 面と psdparse との共通面が決められないため。

### ③ 書き込みの土台 — **実装完了。CSP での確認待ち**

`tools/clip_write.py` に実装。手順は [WRITE_TEST.md](WRITE_TEST.md)。

| | 結果 |
|---|---|
| (a) 無変更往復 | `emptyimage` `opacity` `mask` `test000` `blend2` `tama`(60MB) の 6 件で **sha256 一致** |
| (b) 不透明度だけ変更 | SQLite の UPDATE のみ。ファイルサイズ不変。自前リーダで確認済み |
| (c) サムネイル削除 | チャンク削除 + `ExternalChunk.Offset` 全再計算。整合性 OK、合成結果は max=0 |

**CSP 実機で 3 ファイルとも問題なしを確認済み** (2026-08-16)。つまり:

- CSP は**我々が書いた SQLite を受け入れる** → W1 (属性編集: 名前 / 表示 /
  不透明度 / 合成モード / クリッピング) は実用段階に入った
- **オフセット再計算も正しい** → W2 (画素差し替え) の土台ができた
- **CSP はサムネイルを再生成してくれる** → レイヤ追加で 100% ミップだけ作れば済み、
  サムネイル生成が不要になる (実装が大幅に楽になる)

> 罠: `ExternalChunk.ExternalID` は **BLOB 宣言だが値は TEXT**。
> bytes を束縛して UPDATE すると 1 行もマッチせず黙って失敗する。

### ④ C++ 移植 — **P1/P2 完了**

`clipparse/` に C++17 で実装。`cmake -S . -B build && cmake --build build --config Release`。

- 依存は **zlib + sqlite3 のみ**。両方 `FetchContent` がソース取得するので
  パッケージマネージャ不要 (psdparse と同じ方針)
- SQLite は **`sqlite3_deserialize(..., SQLITE_DESERIALIZE_READONLY)` で
  mmap 上をゼロコピー参照**。一時ファイルを作らない
- 画素が **Python 版とバイト完全一致**することを 4 表現色すべてで確認
  (RGBA / 8bpp グレー / 1bpp モノクロ / マスク)
- `clip_cli --check` が本番ファイルで異常ゼロ。**haruse (91MB / 141,210 ブロック) で 2.6 秒**

### ⑤ CLIP → PSD 変換 — Python 版が動作

`tools/clip_to_psd.py`。psdparse の Python バインディングへ流し込む。

```powershell
python tools/clip_to_psd.py samples/blend2.clip out.psd --verify
```

- 全サンプルで **書き出した PSD を読み直した画素が入力と完全一致**
- PSD の merged image に自前合成を入れているので、
  **Photoshop で開く前でも見た目が正しい** (CanvasPreview との差も自前合成と同じ)
- 合成モードは 27 種を写像。`LayerComposite` → PSD の 4 文字キー

- **レイヤフォルダを保つ**。パススルーは PSD の `pass` へ写像。
  入れ子も保つ (tama.clip で 72 層 / フォルダ 10 を確認)

**制限** (いずれも「完全でなくてよい」の範囲):

1. マスク・クリッピングは**アルファに焼き込む** (見た目は合うが編集不可)
2. 調整レイヤ・ベクタレイヤは出力しない
3. 覆い焼き(発光) の半透明部は α の扱いが違うため非可逆 (警告を出す)

#### フォルダ対応のために psdparse へ入れた変更

隣に置いてある `psdparse` を変更した (あちらでコミット済み):

| ファイル | 変更 |
|---|---|
| `psdparse/psdfile.h` | `addFolder(name, from, count, closed, blendKey, opacity)` 宣言 |
| `psdparse/psdimage.cpp` | `buildFolderExtra()` + `PSDFile::addFolder()` 実装 |
| `python/psdparse_module.cpp` | `add_folder` バインディング |
| `docs/PYTHON_API.md` / `README.md` | 説明を追記 |
| `tests/test_create_folder.py` | 新規テスト 8 件 (psd_tools との相互検証込み) |

PSD のフォルダは「区切りレイヤ (`lsct`=3, 名前 `</Layer group>`) を中身の下、
フォルダレイヤ (`lsct`=1 開 / 2 閉) を中身の上」に置いた 2 枚組。どちらも矩形が空。
`addFolder` は既存の範囲 `[from, from+count)` をその 2 枚で包む。
**下のインデックスは動かない**ので、下から積んで「中身が揃った時点で包む」だけで済む。

テストは `190 passed, 15 skipped` (既存への影響なし)。
`psd_tools` (独立実装) でもグループとして正しく読めることを確認済み。

venv には導入済み (`pip install .` で **0.8.1 → 0.10.0** へ更新。
`PYTHONPATH` 無しで `clip_to_psd.py` がフォルダを作る)。
別環境で使うときは psdparse で `pip install .` を実行すること
— `add_folder` が無い psdparse では自動的に平坦化へ落ちる。

### ⑥ 共通 API — Python 側は完了

判断の根拠と設計は [DESIGN.md](DESIGN.md) §5。結論は
「psdparse スタイルで揃える。ただし共通面は小さく・ツリー主体」。

**psdparse 側 (済)**: `PSDFile.roots` / `children(i)` / `LayerInfo.children` /
`is_group` を追加。平坦リストは正のまま、ツリーは派生ビュー。
区切りレイヤは children に出さない。

**clipparse 側 (済)**: `tools/imgdoc.py` が psdparse 互換の読み取り面を被せる。
`tools/run_on_clip.py` で **psdparse の examples/tools を無改造で .clip に向けられる**:

```
psd_export.py     全サンプルで layers.json + merged.png + レイヤ PNG を出力
composite.py      text.clip の出力が CanvasPreview と max=0 で一致
extract_layers.py folder.clip から 3 レイヤ + manifest.json
```

`tests/test_imgdoc.py` に 14 件。`python -m pytest tests -q`。

### ⑦ C++ の共通面・部分読み・Python 拡張 — 完了

**C++ に合成器を実装** (`clipparse/clipcomposite.cpp`)。Python の参照実装を写した。
20 サンプルで**合成結果がバイト一致**し、`CanvasPreview` との一致度も
Python 版と同等以上。**tama.clip (60MB) の合成が 119 秒 → 5.8 秒 (約 20 倍)**。

> 丸めは numpy の `np.round` に合わせて**偶数丸め**にしてある。
> `floor(x+0.5)` だとちょうど .5 の画素だけ 1 ずれて参照実装と食い違う。

**部分読み** `layerRegion(i, rect)` を実装。**重なる 256x256 タイルだけを展開する**。
PSD は行 RLE なので同じことができない、CLIP 固有の利点。

**Python 拡張** (`python/clipparse_module.cpp`, pybind11)。

```powershell
cmake -S . -B build-py -DCLIPPARSE_BUILD_PYTHON=ON `
      -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
cmake --build build-py --config Release
$env:PYTHONPATH="$PWD\build-py\python\Release"
```

`tools/imgdoc.py` は拡張があればそちらを使う (`imgdoc.BACKEND`)。
両バックエンドの一致は `tests/test_imgdoc.py::test_backends_agree` が見ている。

> 罠: リポジトリ直下の `clipparse/` (C++ ソース) が namespace package として
> import できてしまう。`hasattr(_cpp, "ClipFile")` まで確かめる必要がある。

### ⑧ W1 (レイヤ属性の編集) / W2 (画素の差し替え) / W3 (レイヤ追加) — 実装完了

```powershell
# W1: 属性。SQLite の UPDATE のみ (ExternalChunk.Offset は動かない)
python tools/clip_write.py set IN.clip OUT.clip --layer 5 --opacity 64 --name 新名 --composite 2

# W2: 既存レイヤの画素を PNG で丸ごと差し替え
python tools/clip_write.py setpixels IN.clip OUT.clip --layer 3 --png patch.png

# W3: 既存レイヤを雛形にレイヤを 1 枚足す (--png 省略で空レイヤ)
python tools/clip_write.py addlayer IN.clip OUT.clip --copy-from 3 --name 追加 --png patch.png
```

**W2 の中身**は `tools/clip_encode.py`。`clip_lazy_demo.decode_block` の逆写像で、
アルファ面の 4x4 畳み込み → zlib → ブロックサブレコード → `BlockSize[]` の
書き戻し → `ExternalChunk.Offset` 全再計算まで。
encode→decode 往復が合成画像・実ブロックとも**完全一致**する。

**W3 の方針**: `Layer` は 57 列、`LayerThumbnail` は 43 列あり、CSP が期待する
既定値の大半は意味が分からない。**列挙せず既存行を丸ごと複製**して、ID と
リンクと画素だけ差し替える。`MainId` は `ElemScheme.MaxIndex` から採番し、
ミップ 3 段 (`Mipmap` + `MipmapInfo` + `Offscreen`) とサムネイルも複製、
`LayerFirstChildIndex` / `LayerNextIndex` の兄弟連鎖へ挿し込む。
画素は 100% 段にだけ書く (縮小段に画素が無いのは実ファイルでも同じ)。

読み戻しは自前リーダ・C++ 実装とも入力 PNG と **max=0**、
`clip_cli --check` も 11 ファイルすべて異常 0。
**CSP 実機での確認は [WRITE_TEST_2.md](WRITE_TEST_2.md) 待ち。**

> **未解決: `BlockCheckSum` の算法。** 画素ありブロックは必ず非ゼロ
> (17,185/17,185)、空ブロックは必ずゼロ (148,874/148,874)。CRC32 / ~CRC32 /
> Adler32 / Fletcher32 / FNV-1a / CRC32C / 各種 sum・xor を展開後・圧縮後・
> レコード全体に対して総当たりしたが**どれも一致しない**。CSP 独自と見ている。
> `--checksum zero|crc32|none` の 3 通りで書き分けられるようにして、
> **CSP が検査しているかどうかを実機で切り分ける**方針にした。

### ⑨ W4: PSD → CLIP 変換 — 実装完了

```powershell
python tools/psd_to_clip.py in.psd out.clip --verify
python tools/psd_to_clip.py in.psd out.clip --paper      # 用紙レイヤを残す
```

`tools/clip_build.py` が土台。**空の `.clip` を雛形にキャンバスごと作り替える**:

1. `samples/emptyimage.clip` を開く
2. `resize_canvas` でキャンバスを PSD の寸法へ (ミップ連鎖の段数・各段の
   `Attribute` を全部作り直す)
3. PSD のツリーを下から積む (フォルダも作る)
4. 雛形の 用紙 / レイヤー 1 を消す

**ミップ段数の実測則** (5 ファイルで一致、`docs/CLIP_FORMAT.md` §2.3):
100% から `//2` で縮小し、**グリッドが 1x1 になった段の次の段まで**。
サムネイルはキャンバス寸法によらず **512x512 固定**。

| | |
|---|---|
| 往復 (CLIP → PSD → CLIP) | サンプル 30 本中 **20 本で合成結果がバイト一致**。残り 10 本の差は**全て調整レイヤ** (CLIP→PSD の既知の制限) と覆い焼き(発光)のα で説明が付く |
| 大物 | `tama.clip` (72 層 / フォルダ 10 / 60MB) が往復し、レイヤ画素 max=0・ツリー一致 |
| `Attribute` の作り替え | 実ファイルの 255 個を同じ寸法で作り直すと**バイト一致** |

**CSP 実機での確認は [WRITE_TEST_3.md](WRITE_TEST_3.md) 待ち。**

#### 往復の途中で見つかった不具合 (いずれも修正済み)

1. **psdparse が「カラー比較 (明)」の PSD キーを `ltCl` としていた**。
   正しくは **`lgCl`** (psd_tools・仕様書と一致)。読み込みで
   `BLEND_MODE_INVALID` になり、往復でモードが「通常」に落ちていた。
   `psdparse/psddata.h` で両方受けるようにした
2. **チャンネル順**。psdparse も CLIP と同じ **BGRA** で返すのに RGBA として
   扱っていた。検証側も同じ間違いをしていて**互いに打ち消し合い max=0 に見えていた**
   — 合成結果を元ファイルと突き合わせて初めて出た
3. **不透明度の往復で 1 ずれる**。CLIP 0..256 / PSD 0..255 は段数が違うので
   厳密には 1 対 1 にならない。`clip_to_psd` の切り捨てに対して
   **切り上げ**を返すと `c=1` 以外は元に戻る

### ⑩ CSP 実機確認 1 巡目の結果 (2026-08-17)

[WRITE_TEST_2.md](WRITE_TEST_2.md) / [WRITE_TEST_3.md](WRITE_TEST_3.md) の報告から
**3 件の原因が確定**した。修正済み。再確認は [WRITE_TEST_4.md](WRITE_TEST_4.md)。

| 症状 | 原因 |
|---|---|
| 追加したレイヤが**全面透明** / PSD→CLIP のレイヤが空 / CSP が落ちる | **`Offscreen.BlockData` は BLOB、`ExternalChunk.ExternalID` は TEXT**。同じ ID なのに格納型が逆。新規行に str を入れていて CSP が実体を解決できなかった |
| `_crc32` だけ「レイヤ画像またはレイヤーマスクが破損しています」 | **CSP は非ゼロの `BlockCheckSum` を実際に照合している**。0 は「検査値なし」扱いで通る → **算法を解かずに 0 固定で実用になる** |
| 差し替えたレイヤのサムネイルが古いまま | **実体が残っていると CSP は作り直さない** (無ければ作り直す)。書き換えたら実体を落とす (`drop_thumbnail`) |

**通ったもの**: W1 の属性編集 (名前 / 不透明度 / 合成モード) は全項目 OK。
W2 の画素差し替えは絵・半透明・透明・ブロック境界すべて OK。
空レイヤの追加 (W7) も OK で、描いて保存して開き直せる。

> 教訓: **自前のリーダは SQLite の型に寛容なので、格納型の間違いを検出できない**。
> `tests/test_write.py` に格納型の回帰テストを入れた。
> 新しいテーブルへ行を足すときは、**必ず実ファイルの `typeof()` を確かめる**こと。

### ⑪ CSP 実機確認 2 巡目の結果 (2026-08-17)

[WRITE_TEST_4.md](WRITE_TEST_4.md) の報告 + ユーザーが CSP で作った
`samples/addlayer_csp.clip` (**CSP が足したレイヤの正解**) との列単位の
突き合わせで、残り 3 件の原因が確定。再確認は [WRITE_TEST_5.md](WRITE_TEST_5.md)。

| 症状 | 原因 |
|---|---|
| w9 / w10 が**読み込み中に落ちる** | **`Mipmap.MipmapCount` を段数に合わせていなかった**。CSP はその数だけ連鎖を辿って存在しない段まで進む。雛形と同じ 5 段になる w8 だけ無事だったのが決め手 |
| サムネイルが古いまま | 実体を消すだけでは足りない。**`LayerThumbnail.Thumbnail*NeedRefresh` (14 列) は世代番号**で、CSP は新規レイヤに **50**、既存に 5 を書く |
| 開いた直後だけ白い | **`CanvasPreview`** (キャンバス全体の PNG) を CSP は開いた直後に表示する。雛形のものが残っていた |
| (自前で発見) | `Canvas.CanvasCurrentLayer` が削除したレイヤを指したままだった |

**通ったもの**: W3 のレイヤ追加は画素・サムネイル・描き込み・保存・削除まで全部 OK。
W4 も雛形と同じ段数になるファイル (w8/w11) は開けて絵も正しい。

> **`tools/clip_validate.py` を新設**。CSP で開く前に参照整合性を機械的に検査する
> (ミップ段数 / 閉路 / 孤児行 / 格納型 / 消えたレイヤへの参照)。
> 実ファイル 33 本と生成物 8 本で通る。**書いたら必ず通すこと。**

#### 書く側の作法 (CSP 実機で確定した分)

1. `Offscreen.BlockData` は **BLOB**、`ExternalChunk.ExternalID` は **TEXT**
2. `BlockCheckSum` は **0** (非ゼロは照合されて「破損」になる)
3. `Mipmap.MipmapCount` は**必ず段数と一致**させる (違うと落ちる)
4. サムネイルは**実体を消し**、`Thumbnail*NeedRefresh` に **50**
5. `CanvasPreview` を**合成し直す**
6. `Canvas.CanvasCurrentLayer` を**生きたレイヤ**に向ける

### ⑫ CSP 実機確認 3 巡目 — **全項目 OK** (2026-08-17)

[WRITE_TEST_5.md](WRITE_TEST_5.md) の全チェックが通った。**書き込み経路は
CSP 5.0.4 で実用段階**:

| | CSP で確認済み |
|---|---|
| W1 属性編集 | 名前 / 不透明度 / 合成モード |
| W2 画素の差し替え | 絵・半透明・透明・ブロック境界・サムネイル |
| W3 レイヤ追加 | 絵・サムネイル・描き込み・上書き保存・削除 |
| W4 PSD → CLIP | フォルダ・合成モード・寸法の作り替え・起動直後の表示・上書き保存 |

**書く側の作法 (CSP 実機で確定)**。どれも**自前のリーダでは検出できない**
種類の間違いなので、`tools/clip_validate.py` に検査を入れてある:

1. `Offscreen.BlockData` は **BLOB**、`ExternalChunk.ExternalID` は **TEXT**
   — 取り違えるとそのレイヤが全面透明になる
2. `BlockCheckSum` は **0** — 非ゼロは照合されて「破損しています」になる
3. `Mipmap.MipmapCount` は**必ず段数と一致** — 違うと読み込み中に落ちる
4. サムネイルは**実体を消し**、`Thumbnail*NeedRefresh` に **50**
5. `CanvasPreview` を**合成し直す** — 開いた直後に表示される
6. `Canvas.CanvasCurrentLayer` を**生きたレイヤ**に向ける

**書いたら必ず `python tools/clip_validate.py OUT.clip` を通すこと。**

### ⑬ 書く側の C++ 移植 — 完了

`clipparse/clipencode.{h,cpp}` / `clipwrite.{h,cpp}` / `clipvalidate.cpp`。
Python 版 (`tools/clip_write.py` + `clip_encode.py` + `clip_build.py` +
`clip_validate.py`) と同じことをする。

```powershell
build\clipparse\Release\clip_cli.exe in.clip --validate
build\clipparse\Release\clip_cli.exe in.clip --roundtrip out.clip
build\clipparse\Release\clip_cli.exe in.clip --set 5 --opacity 64 --out out.clip
build\clipparse\Release\clip_cli.exe in.clip --set-pixels 3 rgba.raw out.clip
build\clipparse\Release\clip_cli.exe in.clip --add-layer  3 rgba.raw out.clip --name 追加
```

Python バインディングもある (`clipparse.ClipWriter` / `clipparse.validate`)。

| | |
|---|---|
| SQLite | `sqlite3_deserialize(RESIZEABLE)` で**書けるメモリ DB** にし、`sqlite3_serialize` で取り出す。一時ファイルを作らない (Python 版は temp ファイル経由) |
| 行の複製 | `PRAGMA table_info` + `sqlite3_bind_value` で**格納型ごと**写す。`Layer` 57 列の既定値を当てずっぽうで書かずに済む |
| PNG | `CanvasPreview` 用に zlib だけで書く最小実装 (フィルタは全行 0) |

**正しさの担保**: C++ と Python が書いたファイルは
**`CHNKExta` のペイロードがバイト一致する** (zlib の出力まで同じ)。
外部 ID は乱数なので比較はペイロードの集合で行う。
`tests/test_write.py` に 7 件足してある。

| | |
|---|---|
| 無変更往復 | `tama.clip` (60MB) で **sha256 一致 / 0.14 秒** (Python は 0.46 秒) |
| 画素の差し替え | 2894x4093 / 192 ブロックで**チャンクがバイト一致 / 0.53 秒** (Python は 1.05 秒。zlib 律速なので差は小さい) |
| 検査 | `clip_cli --validate` が実ファイル 42 本すべてで異常 0 |

> 罠 2 件 (どちらも Python 移植時と同じ):
> **`Offscreen.BlockData` は `sqlite3_bind_blob`** で入れること
> (`bind_text` だと CSP がそのレイヤを全面透明として開く)。
> **Windows の `main` の argv はアクティブコードページ**なので、
> 日本語のレイヤ名は `CommandLineToArgvW` から取り直して UTF-8 に直す。

### ⑭ CLIP ⇄ PSD 変換コマンド (C++) — 完了

`examples/clipconv/`。**clipparse と psdparse の両方を参照する外部サンプル**で、
どちらのライブラリにも専用の口を足していない (公開 API だけで書ける形になった)。

```powershell
cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
cmake --build build-conv --config Release
build-conv\Release\clipconv.exe in.clip out.psd --verify
build-conv\Release\clipconv.exe in.psd out.clip --verify
```

| | |
|---|---|
| 往復 | CLIP → PSD → CLIP で **13 サンプルすべて合成結果がバイト一致** |
| Python 版との一致 | `folder` / `blend2` の PSD が **sha256 一致**、PSD→CLIP のチャンクが**ペイロード一致** |
| 速度 (tama 60MB) | CLIP→PSD **13.9 秒** (Python 2 分 11 秒) / PSD→CLIP **45.6 秒** (同 1 分 22 秒) |

Python 版と 1 箇所だけ違う: **C++ 版はテキストレイヤを外接矩形で PSD へ置く**
(Python 版はキャンバス全面)。C++ 版の方が PSD が小さくなる。合成結果は同じ。

> 調整レイヤを持つファイルは**見た目が往復しない** (`tama.clip` の
> トーンカーブ / カラーバランスなど)。CLIP→PSD が調整レイヤを落とす既知の制限で、
> Python 版も同じ。レイヤの画素とツリーは保たれる。

### ⑮ Python パッケージ化と公開 — 完了 (PyPI 公開済み)

`pyproject.toml` (scikit-build-core)。`pip install .` / `pip wheel .` /
`python -m build --sdist` が通る。psdparse と同じ流儀。

| | |
|---|---|
| 名前 | `clipparse` — **PyPI は空いている** (2026-08-17 時点で 404) |
| 版 | 0.1.0 |
| ライセンス | MIT (`LICENSE`。psdparse と同文) |
| 依存 | **無し**。ホイールの中身は C++ 拡張 1 つだけ (750 KB) |
| sdist | 175 KB。`samples/` と `*.clip` は除外される |

**`tools/imgdoc.py` は同梱しない**方針にした。あれは psdparse を要求するので、
入れると依存ゼロでなくなる。psdparse 互換面が要る人はリポジトリから取る。

クリーンな venv にホイールを入れて、読み・合成・書き・検査まで動作確認済み。

**公開済み** (2026-08-17):

| | |
|---|---|
| PyPI | https://pypi.org/project/clipparse/ — 0.1.0、**36 ホイール + sdist** |
| GitHub | https://github.com/wamsoft/clipparse (public、`wamsoft` org) |
| CI | `.github/workflows/wheels.yml` (cibuildwheel)。psdparse から移植 |
| 対応 | Python 3.9〜3.14 (free-threaded 含む) × Linux / Windows / macOS (x86_64 + arm64) |

公開の手順 (次回のため):

1. PyPI と TestPyPI の**アカウント設定**で「pending publisher」を登録する
   (プロジェクトがまだ無いので、プロジェクト設定側からは登録できない)。
   Owner `wamsoft` / Repository `clipparse` / Workflow **`wheels.yml`** /
   Environment `pypi` (TestPyPI は `testpypi`)
2. GitHub の Settings → Environments に `pypi` / `testpypi` を作る
3. `Actions → wheels → Run workflow` で TestPyPI へ予行演習
4. `git tag -a vX.Y.Z && git push origin vX.Y.Z` で本番公開

> **pending publisher は名前を予約しない。** 最初の publish が通るまでは
> 他人に名前を取られうるので、登録したら間を空けずに公開まで進めること。

> **CI を回して初めて出た不具合**: `clipbase.h` が `memcpy` を使うのに
> `<cstring>` を include していなかった。MSVC と Apple clang は間接的に
> 入るので Windows / macOS では通り、**manylinux の GCC だけで落ちた**。
> 3 プラットフォームでビルドする価値はここにある。

### ⑯ 残っているもの

- **彩度 (合成モード 24) の残差 8**、**色相・彩度・明度の彩度/明度の式**
- **クリッピングの縁 168 画素**
- **PSD → CLIP でマスク・テキスト・調整レイヤを「編集可能なまま」持ち込む**
  (現状はラスタとして焼き込まれる)
- **ベクタ**はブラシエンジンが要るので当面やらない
- C++ 側の `imgdoc` 相当 (純粋仮想の共通面)。Python で形が固まったので写せるが、
  C++ で両形式を混ぜて使う具体的な需要が出てから作るのでも遅くない

## リポジトリの状態

`master` にコミット済み。`samples/` と `build*/` は gitignore。

```
CLAUDE.md                    リポジトリ運用の指針
README.md                    概要 (英語・利用者向けの入口)
README.ja.md                 同 日本語版
docs/PYTHON_API.md           Python API リファレンス (英語)
docs/PYTHON_API.ja.md        同 日本語版
.gitignore                   samples/ と *.clip を除外
docs/CLIP_FORMAT.md          形式仕様 ([実測]/[推定] を区別)
docs/DESIGN.md               設計検討・ロードマップ・テンプレート方式(§6.1)
docs/CLIP_TOOLS_REPORT.md    clip-tools への報告 (英語、未送付)
docs/SAMPLE_REQUESTS.md      CSP サンプル依頼 第 1 弾 (完了)
docs/SAMPLE_REQUESTS_2.md    CSP サンプル依頼 第 2 弾 (完了)
docs/SAMPLE_REQUESTS_3.md    CSP サンプル依頼 第 3 弾 (完了)
docs/WRITE_TEST.md           書き込み経路の CSP 確認手順 (往復/属性/削除 — 確認済み)
docs/WRITE_TEST_2.md         同 その 2 (画素差し替え/レイヤ追加 — 確認待ち)
docs/WRITE_TEST_3.md         同 その 3 (PSD -> CLIP 変換 — 1 巡目完了)
docs/WRITE_TEST_4.md         同 その 4 (2 巡目 — 完了)
docs/WRITE_TEST_5.md         同 その 5 (3 巡目 — 全項目 OK)
docs/STATUS.md               このファイル
examples/clipconv/           CLIP <-> PSD 変換コマンド (psdparse も参照する外部サンプル)
clipparse/                   C++17 本体
  clipfile / clipcomposite     読む側 (遅延参照・合成)
  clipencode / clipwrite       書く側 (ブロック生成・レイヤ編集・PNG)
  clipvalidate                 参照整合性の検査
  clip_cli                     読み書き両方の CLI
CMakeLists.txt               zlib + sqlite3 を FetchContent で取得
pyproject.toml               Python パッケージ設定 (scikit-build-core, 依存なし)
LICENSE                      MIT
tools/clip_probe.py          構造ダンプ (標準ライブラリのみ)
tools/clip_lazy_demo.py      遅延参照プロトタイプ + 回帰テスト (仕様の基準)
tools/clip_write.py          書き出し (往復 / 属性編集 / 画素差し替え / レイヤ追加)
tools/clip_encode.py         ピクセルブロックを書く側 (decode_block の逆写像)
tools/clip_build.py          キャンバスの寸法ごと作り替える (W4 の土台)
tools/psd_to_clip.py         PSD -> CLIP 変換
tools/clip_validate.py       参照整合性の検査 (CSP で開く前に通す)
tools/clip_to_psd.py         CLIP -> PSD 変換
tools/imgdoc.py              psdparse 互換の読み取り面 (C++/Python 両バックエンド)
tools/run_on_clip.py         psdparse 向けスクリプトを .clip に向ける実行器
tests/test_imgdoc.py         共通面のテスト 16 件
tests/test_write.py          書き込み経路のテスト 25 件 (C++ との突き合わせ込み)
python/clipparse_module.cpp  pybind11 バインディング
samples/                     gitignore 済み。実ファイル置き場
```

### samples/ の中身

| ファイル | 出所 | 扱い |
|---|---|---|
| `test000.clip` / `test000.png` | clip-tools (MIT) | **回帰フィクスチャ。消さない** |
| `emptyimage.clip` / `emptyanime.clip` | ユーザーが CSP で新規作成 | テンプレート方式の種 |
| `tama.clip` / `haruse-ja.clip` / `nazoani01_ja.clip` | CLIP STUDIO 公式サンプル | **再配布不可・コミット禁止** |
| `gray_*` `mono_*` `text` `vector*` `mask` `clipping` `folder` `filter` `opacity` `blendmodes` `blend2` `passthrough` `adj_*` | ユーザーが CSP 5.0.4 で作成 | 合成の正解合わせ用 |

---

## 参考: clip-tools 本体を動かしたいとき

`https://github.com/animeops/clip-tools` を clone する。依存は
pandas / numpy / Pillow / opencv / scikit-image / tqdm。

パーサ単体だけ試すなら全部入れる必要はなく、
`clip_tools/utils.py` の `read_binary_spec` (struct のみで書かれている) を
スタブして対象モジュールを `importlib` で直接ロードすればよい。
[CLIP_TOOLS_REPORT.md](CLIP_TOOLS_REPORT.md) 末尾に手順あり。
