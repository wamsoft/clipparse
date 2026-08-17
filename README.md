# clipparse

CLIP STUDIO PAINT の `.clip` ファイルを、[psdparse](../psdparse) と同じ思想
— **構造メタ情報だけを保持し、実データは元ファイルから必要になった時に読む** —
で扱うライブラリ。

仕様解析は完了。C++ 本体 (`clipparse/`) と Python 検証実装 (`tools/`) が動作中。
到達点と再開手順は [docs/STATUS.md](docs/STATUS.md) にまとめてある。

## ドキュメント

| | |
|---|---|
| [docs/CLIP_FORMAT.md](docs/CLIP_FORMAT.md) | CLIP ファイル形式の仕様。実ファイルで検証済みの部分と、[clip-tools](https://github.com/animeops/clip-tools) 由来の推定部分を区別して記述 |
| [docs/DESIGN.md](docs/DESIGN.md) | 遅延参照方式の実現性検討、psdparse との共通 API 案、変換処理の見通し、実装ロードマップ |
| [docs/STATUS.md](docs/STATUS.md) | **まずここ。** 到達点・再開手順・次にやること |
| [docs/WRITE_TEST.md](docs/WRITE_TEST.md) 〜 [_5.md](docs/WRITE_TEST_5.md) | 書き込み経路の CSP 実機確認 (全 5 巡・**全項目 OK**) |
| [docs/CLIP_TOOLS_REPORT.md](docs/CLIP_TOOLS_REPORT.md) | clip-tools へのフィードバック用 (英語)。再現手順付きのバグ 3 件 + 仕様の訂正 |

## 調査結果の要点

1. `.clip` は `[バイナリ領域][SQLite3 DB]` の 2 部構成。メタ情報は全て SQLite 側にあり、
   実ピクセルは外部チャンク (`CHNKExta`) として 256x256 ブロック + zlib で入っている。
2. **SQLite の `ExternalChunk.Offset` と `Offscreen.Attribute.BlockSize[]` の累積和だけで、
   任意のピクセルブロックの絶対オフセットが確定する。** バイナリ領域の走査は不要。
   → psdparse 型の遅延読みが素直に成立し、しかも **256x256 ブロック単位**で
   部分読みできる (PSD より細かい粒度)。
3. メタ情報は最初から関係データベースなので、「動的なテーブルビュー + 型付きビュー」の
   2 段構えにできる。CSP のバージョン差で列が増減しても基層は壊れない。
4. `Offscreen.Attribute` の `BlockSize[i]` は圧縮サイズではなく
   **サブレコード全体の長さ** (clip-tools の理解を実測で訂正)。空ブロックは常に 104 バイト。
5. レイヤは **描画用とマスク用の 2 本のミップ連鎖**を持ち、どちらも同じ `LayerId`・
   同じ `ThisScale=100.0` で始まる。`Layer.LayerRenderMipmap` → `Mipmap.BaseMipmapInfo`
   から辿らないとマスクをレイヤ画像として読んでしまう。
6. **書く側には「読めてしまうが CSP が受け付けない」落とし穴がある** — 同じ ID なのに
   テーブルごとに BLOB / TEXT と格納型が違う、`Mipmap.MipmapCount` が段数と食い違うと
   CSP が落ちる、など。実機で 5 巡かけて洗い出した結果を
   [docs/CLIP_FORMAT.md](docs/CLIP_FORMAT.md) に記録し、
   `tools/clip_validate.py` で機械的に検査できるようにしてある。

## 検証状況

- 構造アサーション: `test000` + 本番 3 ファイル (60〜91 MB) で **164,562 ブロック**成立
- 合成の再現: **28 サンプル中 13 が CSP 出力とピクセル完全一致、22 が丸め誤差以内**。
  合成モード 27 種・表現色 4 種・マスク・クリッピング・通過フォルダ・
  調整レイヤ 5 種を実測同定して実装 (詳細は CLIP_FORMAT.md §9/§10)
- 書き込み: 無変更往復が **sha256 一致** (60MB ファイルでも)。
  **属性編集・画素の差し替え・レイヤ追加・PSD からの新規作成のすべてを
  CSP 5.0.4 実機で確認済み** (docs/WRITE_TEST*.md)
- 相互変換: CLIP → PSD → CLIP の往復で、サンプル 30 本中 **20 本が合成結果まで
  バイト一致**。残り 10 本の差は調整レイヤ (PSD に出さない既知の制限) で説明が付く
- C++ 版: 画素が Python 版と**バイト完全一致**。91MB / 141,210 ブロックを 2.6 秒。
  **書く側も移植済み**で、C++ と Python の出力はチャンクのバイト列まで一致する

## C++ ライブラリ

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

build\clipparse\Release\clip_cli.exe samples\test000.clip
build\clipparse\Release\clip_cli.exe samples\test000.clip --check
```

依存は **zlib と sqlite3 のみ**。どちらも CMake の `FetchContent` がソースから
取ってくるので、vcpkg 等のパッケージマネージャは不要 (psdparse と同じ方針)。
SQLite は `sqlite3_deserialize(..., SQLITE_DESERIALIZE_READONLY)` で
mmap 上をゼロコピー参照する — 一時ファイルを作らない。

読み出しも合成も、Python の参照実装と**画素バイト単位で一致**することを
回帰で確認している (表現色 4 種 / 合成モード 27 種 / 調整レイヤ / フォルダ)。
**60 MB のファイルの合成で純 Python 版の約 20 倍速** (5.8 秒 vs 119 秒)。

C++ 側にしかない口:

- `layer_region(i, x, y, w, h)` — **重なる 256x256 タイルだけを展開する**部分読み。
  PSD は行 RLE なので同じことができない
- `preview_png()` — ファイルに埋まっている完成画 (`CanvasPreview`)

### 書く側 (C++)

`clip::ClipWriter` が `tools/clip_write.py` と同じことをする。

```cpp
clip::ClipWriter w;
w.load("in.clip");
w.addLayer(3, "追加", rgba, 300, 400);   // 既存レイヤを雛形に複製する
w.save("out.clip");
```

```powershell
build\clipparse\Release\clip_cli.exe in.clip --validate
build\clipparse\Release\clip_cli.exe in.clip --roundtrip out.clip
build\clipparse\Release\clip_cli.exe in.clip --set 5 --opacity 64 --out out.clip
build\clipparse\Release\clip_cli.exe in.clip --set-pixels 3 rgba.raw out.clip
build\clipparse\Release\clip_cli.exe in.clip --add-layer  3 rgba.raw out.clip --name 追加
```

SQLite は `sqlite3_deserialize(RESIZEABLE)` で**書けるメモリ DB** にして、
`sqlite3_serialize` で取り出す。一時ファイルを作らない。無変更往復は
60 MB のファイルでも **sha256 一致** (0.14 秒)。

**C++ と Python が書いたファイルは `CHNKExta` のペイロードがバイト一致する**
(zlib の出力まで同じ)。これが書く側の正しさの担保で、
`tests/test_write.py` が自動で確かめている。

`clip::validate` / `clip_cli --validate` が参照整合性を検査する。
**CSP で開く前に必ず通すこと** — ここに引っかかる種類の間違いは、
寛容なリーダでは読めてしまうのに CSP では落ちたり全面透明になったりする。

## CLIP ⇄ PSD 変換コマンド (C++)

`examples/clipconv/` に**両方のライブラリを参照する側**のサンプルがある。
どちらのライブラリもこのコマンドのために特別な口を持っていない
— 公開 API だけで書いてある。

```powershell
cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
cmake --build build-conv --config Release

build-conv\Release\clipconv.exe in.clip out.psd  --verify
build-conv\Release\clipconv.exe in.psd  out.clip --verify
```

CLIP → PSD → CLIP の往復で **13 サンプルすべて合成結果がバイト一致**。
`tama.clip` (60MB / 72 層) で CLIP → PSD が **13.9 秒** (Python 版は 2 分 11 秒)、
PSD → CLIP が **45.6 秒** (同 1 分 22 秒)。
詳細と制限は [examples/clipconv/README.md](examples/clipconv/README.md)。

## psdparse 互換で読む

`tools/imgdoc.py` が psdparse 互換の読み取り面を `.clip` に被せる。
**psdparse の `examples/` や `tools/` を 1 行も直さずに `.clip` へ向けられる。**

```powershell
# psdparse 向けのスクリプトを、そのまま .clip に対して走らせる
python tools/run_on_clip.py D:	est\psdparse	ools\psd_export.py sampleslend2.clip --out-dir out
python tools/run_on_clip.py D:	est\psdparse\examples\composite.py samples	ext.clip out.png
```

```python
import imgdoc
doc = imgdoc.open("file.clip")     # .psd なら psdparse.PSDFile をそのまま返す
doc.header.width, doc.header.height
for i in doc.roots:                 # ツリービュー
    print(doc.layers[i].name_unicode, doc.layers[i].children)
doc.layer_image(0)                  # BGRA bytes
```

`layer_type` / `blend_mode` は **psdparse の enum をそのまま返す**ので、
`psdparse.LayerType.NORMAL` との比較がそのまま通る。
設計判断の根拠は [docs/DESIGN.md](docs/DESIGN.md) §5。

バックエンドは C++ 拡張があればそちら、無ければ純 Python の参照実装。
どちらでも結果は同じ (`imgdoc.BACKEND` で分かる)。

## 検証ツール

```
# 構造ダンプ (チャンク配置 / テーブル / Layer ツリー / Offscreen / ブロック列)
python tools/clip_probe.py path/to/file.clip [--blocks]

# 遅延参照プロトタイプ: SQLite から計算したオフセットだけで画素を取り出し、
# 下から順に合成して参照 PNG と比較する
python tools/clip_lazy_demo.py file.clip -o out.png --compare reference.png
```

```
# CLIP <-> PSD 変換 (psdparse の Python バインディングが要る)
python tools/clip_to_psd.py input.clip output.psd --verify
python tools/psd_to_clip.py input.psd  output.clip --verify

# 書き出し (無変更往復はバイト一致するのが正)
python tools/clip_write.py roundtrip in.clip out.clip
python tools/clip_write.py set       in.clip out.clip --layer 5 --opacity 64 --composite 2
python tools/clip_write.py setpixels in.clip out.clip --layer 3 --png patch.png
python tools/clip_write.py addlayer  in.clip out.clip --copy-from 3 --name 追加 --png patch.png

# 参照整合性の検査。**CSP で開く前に必ず通す**
python tools/clip_validate.py out.clip
```

> 書く側には「こちらのリーダでは読めるのに CSP が受け付けない」落とし穴が
> いくつもある (格納型・`MipmapCount`・チェックサム・サムネイルの世代番号・
> `CanvasPreview`)。CSP 実機で 5 巡かけて洗い出し、
> `clip_validate.py` で機械的に検査できるようにしてある。

`clip_probe.py` は標準ライブラリのみ。`clip_lazy_demo.py` は numpy (比較時のみ Pillow)。

`samples/` の実ファイルはリポジトリ管理外 (`.gitignore`)。

## 一次資料

- [animeops/clip-tools](https://github.com/animeops/clip-tools) — Python 実装。
  形式解析の出発点。特に `clip_tools/clip.md` と `clip_tools/structs/`。
- [psdparse](../psdparse) — 本ライブラリが倣う設計。`docs/ARCHITECTURE.md` を参照。
