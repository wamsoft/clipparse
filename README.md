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
| [docs/WRITE_TEST.md](docs/WRITE_TEST.md) | 書き込み経路の CSP 実機確認 (完了済み) |
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

## 検証状況

- 構造アサーション: `test000` + 本番 3 ファイル (60〜91 MB) で **164,562 ブロック**成立
- 合成の再現: **28 サンプル中 13 が CSP 出力とピクセル完全一致、22 が丸め誤差以内**。
  合成モード 27 種・表現色 4 種・マスク・クリッピング・通過フォルダ・
  調整レイヤ 5 種を実測同定して実装 (詳細は CLIP_FORMAT.md §9/§10)
- 書き込み: 無変更往復が **sha256 一致** (60MB ファイルでも)。
  属性編集・チャンク再配置したファイルを **CSP が問題なく開くことを実機確認済み**
- C++ 版: 画素が Python 版と**バイト完全一致**。91MB / 141,210 ブロックを 2.6 秒

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
# CLIP -> PSD 変換 (psdparse の Python バインディングが要る)
python tools/clip_to_psd.py input.clip output.psd --verify

# 書き出し (無変更往復はバイト一致するのが正)
python tools/clip_write.py roundtrip in.clip out.clip
python tools/clip_write.py opacity   in.clip out.clip --layer 5 --value 64
```

`clip_probe.py` は標準ライブラリのみ。`clip_lazy_demo.py` は numpy (比較時のみ Pillow)。

`samples/` の実ファイルはリポジトリ管理外 (`.gitignore`)。

## 一次資料

- [animeops/clip-tools](https://github.com/animeops/clip-tools) — Python 実装。
  形式解析の出発点。特に `clip_tools/clip.md` と `clip_tools/structs/`。
- [psdparse](../psdparse) — 本ライブラリが倣う設計。`docs/ARCHITECTURE.md` を参照。
