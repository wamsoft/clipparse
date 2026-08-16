# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## リポジトリの現状

**まず [docs/STATUS.md](docs/STATUS.md) を読むこと。** 到達点・再開手順・次にやることが
そこに集約してある。

目的: `.clip` (CLIP STUDIO PAINT) を **psdparse と同じ遅延参照方式** で読むライブラリを作る。
「メタ情報だけ常駐させ、実データは元ファイルから必要になった時に読む」。

構成:

- `clipparse/` — **C++17 の本体** (実装中)。依存は zlib + sqlite3 のみ
- `tools/` — Python の検証実装。**仕様の正しさはこちらが基準**
- `docs/` — 仕様書と設計。まず `docs/STATUS.md`

ロードマップは `docs/DESIGN.md` §8。

## コマンド

```powershell
# C++ ビルド (依存は zlib + sqlite3。FetchContent が自動取得)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
build\clipparse\Release\clip_cli.exe samples\test000.clip --check

# 書き込み経路 (無変更往復はバイト一致するのが正)
python tools/clip_write.py roundtrip samples/opacity.clip out.clip

# 回帰テスト (これが唯一。ピクセル完全一致なら exit 0)
python tools/clip_lazy_demo.py samples/test000.clip --compare samples/test000.png

# 構造ダンプ (標準ライブラリのみ)
python tools/clip_probe.py path/to/file.clip
python tools/clip_probe.py path/to/file.clip --blocks    # ブロックサブレコードまで展開
```

サンプルは `samples/` (gitignore 済み)。**参照 PNG があるのは `test000` だけ**なので、
ピクセル一致の回帰確認はこれで行う。公式サンプル 3 本 (tama / haruse / nazoani) は
**再配布不可**。中身の一覧は `docs/STATUS.md`。

## 形式の要点 (詳細は docs/CLIP_FORMAT.md)

```
[CSFCHUNK ヘッダ][CHNKHead][CHNKExta ...][CHNKSQLi][CHNKFoot]
                            ↑実データ      ↑全メタ情報 (SQLite3 DB)
```

- 全整数はビッグエンディアン。**例外はブロックの zlib 圧縮長のみリトルエンディアン。**
- マーカー文字列は UTF-16BE。
- `CHNKHead.binary_section_size` が `CHNKSQLi` チャンクの位置と一致するので、
  **先頭 64 バイトだけ読めば SQLite に直行できる**。
- 実データの所在は `ExternalChunk(ExternalID → Offset)` が持つ。
  **`Offscreen.Attribute.BlockSize[]` に値が入っていても実体があるとは限らない** —
  実体の有無は `ExternalChunk` に載っているかだけで判定する。
- `Offscreen.Attribute.BlockSize[i]` は **サブレコード全長** (圧縮サイズではない)。
  空ブロック = 104 バイト固定、データあり = 圧縮長 + 112。
  前置和を取れば任意ブロックの位置が O(1) で出る。
- レイヤツリーは `LayerFirstChildIndex` / `LayerNextIndex` のリンクリスト。
  ルートは `Canvas.CanvasRootFolder`。**子チェーンの先頭が最下層。**
- **ミップ連鎖は `Layer.LayerRenderMipmap` → `Mipmap.BaseMipmapInfo` →
  `MipmapInfo.NextIndex` で辿る。** `MipmapInfo` を `(LayerId, ThisScale=100)` で
  引いてはいけない — マスク用連鎖が同じ条件で引っかかる。
- `Offscreen.Attribute` の `InitColor` 末尾は **可変長** (`4 * quad[2]` バイト)。
  `has_color` を見て 16 バイト固定で読むとマスク面で壊れる。
- `LayerOpacity` は **0..256** (255 ではない)。
- ピクセルブロックは `(block_h + 64, block_w, 4)`。rows[64:] が B,G,R,(未使用)、
  rows[0:64] が 4x4 スーパーピクセルに畳まれたアルファ面。**ストレートアルファ**。

## 作業上の注意

- **仕様書に書く事実は必ず実ファイルで裏を取る。** `docs/CLIP_FORMAT.md` は
  **[実測]** (このリポジトリで確認済み) と **[推定]** (clip-tools 由来の未確認ラベル) を
  区別している。この区別を崩さないこと。推測を [実測] に格上げするときは
  `tools/clip_probe.py` の出力を根拠として示す。
- 新しいサンプルが手に入ったら、まず `clip_probe.py --blocks` と
  `clip_cli --check` を通してアサーションが崩れないか見る。
  未検証の箇所は `docs/CLIP_FORMAT.md` §7 に一覧がある。
- 変更を入れたら `clip_lazy_demo.py --compare` がピクセル一致 (exit 0) のままか確認する。
  これが現状唯一の回帰テスト。
- **C++ を変更したら Python 版と画素バイト一致するか確かめる。** これが C++ 側の
  唯一の正しさの担保 (`clip_cli --dump-offscreen ID out.raw` と
  `clip_lazy_demo.ClipFile.offscreen_image()` を突き合わせる)。
- `ExternalChunk.ExternalID` は **BLOB 宣言だが値は TEXT**。bytes を束縛して
  UPDATE すると 1 行もマッチせず黙って失敗する (`tools/clip_write.py` の教訓)。
