# clipparse

[English README](README.md) — このページの英語版。

CLIP STUDIO PAINT の `.clip` を**読む・合成する・編集する・書く** C++17 ライブラリ。
pybind11 の Python バインディングを PyPI に公開している。

- **遅延読み込み。** 解析で触るのは埋め込みの SQLite (メタ情報) だけ。画素は
  **256x256 ブロック単位**で、必要になった時に mmap 上から展開する。
- **部分読み。** `layer_region()` は指定した矩形に**重なるタイルだけ**を展開する。
  行 RLE の PSD には真似のできない、CLIP 固有の利点。
- **合成。** 合成モード 27 種・フォルダ (通過を含む)・マスク・クリッピング・
  調整レイヤ 5 種を実装。CSP 自身がファイルに保存しているプレビューと
  画素単位で突き合わせて確認してある。
- **書き込み。** 無変更の往復は **sha256 一致** (60 MB のファイルでも)。属性編集・
  画素の差し替え・レイヤの追加削除・キャンバスごとの作り直しを、
  **CLIP STUDIO PAINT PRO 5.0.4 の実機で確認済み**。
- **CLIP ⇄ PSD。** [psdparse](https://github.com/wamsoft/psdparse) 経由の相互変換を
  C++ と Python の両方に用意。
- **実行時の依存なし。** zlib と sqlite3 は取り込み済みで、
  ホイールの中身は拡張モジュール 1 つだけ。

形式は実ファイルからの解析による。**実測で裏を取った事実**と**未確認の推定**は
[docs/CLIP_FORMAT.md](docs/CLIP_FORMAT.md) で明確に分けてある。

## インストール (Python)

```bash
pip install clipparse
```

Python 3.9〜3.14 (free-threaded を含む) × Linux / Windows / macOS (x86_64 + arm64)
のホイールを公開している。ソースから入れる場合も、必要なのは C++17 コンパイラと
CMake 3.16+ だけで、**パッケージマネージャは要らない**:

```bash
pip install .
```

## 使いはじめ

```python
import clipparse

f = clipparse.ClipFile()
f.load("artwork.clip")

print(f.width, f.height, f.resolution)         # キャンバスのピクセル寸法と解像度
for layer in f.layers:                          # 平坦なリスト。下から上
    print(layer.index, layer.name, layer.opacity, layer.is_group)

bgra = f.merged_image()                         # 全レイヤを合成した BGRA バイト列
one  = f.layer_image(2)                         # レイヤ 1 枚の BGRA バイト列
part = f.layer_region(2, 100, 120, 64, 48)      # 重なるタイルだけを展開する部分読み

png, w, h = f.preview_png()                     # CSP が保存したプレビュー画像
```

画素は常に **BGRA バイト列・ストレートアルファ** で返る (psdparse と同じ約束):

```python
from PIL import Image
img = Image.frombytes("RGBA", (f.width, f.height), f.merged_image())
b, g, r, a = img.split()
Image.merge("RGBA", (r, g, b, a)).save("merged.png")
```

書く側。**レイヤの指定は `Layer.MainId` (`layer.main_id`)** で、
読む側で使うリストの添字ではない:

```python
w = clipparse.ClipWriter()
w.load("artwork.clip")

w.set_layer_attr(main_id, name="改名", opacity=128)      # ここでの不透明度は 0..256
w.set_pixels(main_id, bgra, f.width, f.height)           # 画素を丸ごと差し替える
new_id = w.add_layer(main_id, "追加", bgra, f.width, f.height)
w.delete_layer(other_id)

w.save("out.clip")
assert clipparse.validate("out.clip") == []              # CSP で開く前に必ず通す
```

API の詳細は **[docs/PYTHON_API.ja.md](docs/PYTHON_API.ja.md)**
([English](docs/PYTHON_API.md))。

## コマンドラインツール

ツール群はホイールに同梱してあるので、`pip install clipparse` だけで
次のコマンドが使える (追加の依存なし — 合成・画素展開は同梱の C++ 拡張が行う):

```
clip-probe    file.clip [--blocks]        # 構造ダンプ (チャンク / テーブル / ツリー / ブロック)
clip-validate file.clip                   # 参照整合性の検査。CSP で開く前に通す
clip-doctor   file.clip [--deep]          # レイヤ単位の診断。--fix/--remove で不正部分を除去
clip-export   file.clip [-o out.png]      # 合成 PNG。--layers DIR で全レイヤ + manifest
clip-write    roundtrip in.clip out.clip  # 書き出し (往復 / 属性 / 画素 / レイヤ追加)
```

`clip-validate` がファイル全体の合否だけを出すのに対し、`clip-doctor` は
**どのレイヤが悪いか**まで切り分ける:

```
clip-doctor file.clip [--deep]                   # 診断: レイヤツリー + 問題一覧
clip-doctor file.clip --fix --out fixed.clip     # 修復 + 壊れたレイヤの除去
clip-doctor file.clip --remove 7 --out out.clip  # 指定レイヤの除去 (フォルダは子孫ごと)
```

- 全レイヤのミップ連鎖 (描画・マスク両方)、Attribute とブロック列の構造、
  参照している実体チャンクがファイル内に実在するかを検査する。
  `--deep` を付けると全ブロックを zlib 展開して照合する。
- 判定は 3 段階。**除去候補** = レイヤの画素データ自体が壊れている
  (連鎖の断線・ブロック破損・実体の欠落)。直しようがないので `--fix` は
  レイヤごと取り除く。**修復可能** = 参照や数値の食い違いでデータ本体は無事
  (`MipmapCount` 不一致・死んだリンク・格納型・マスクやサムネイルだけの破損)。
  `--fix` がその場で直し、マスク・サムネイルは壊れた部分だけ切除する。
  **情報** = CSP 自身のファイルにもある無害な状態。
- レイヤ番号は `MainId` (ツリー表示と `clip-probe` に出るもの)。
  `clip-export` の index とは別物。
- 書き出し後は内蔵プレビューを合成し直し、`clip-validate` と同じ検査を
  自動で通す。行う手術はすべて CLIP STUDIO PAINT 実機 (PRO 5.0.4) で
  開けることを確認済み。

一部の機能は、対応するライブラリがある環境でだけ有効になる
(extras 方式。本体は依存ゼロのまま):

```
pip install clipparse[psd]    # psdparse + numpy  → clip-to-psd / psd-to-clip 変換
pip install clipparse[image]  # numpy + Pillow    → clip-write setpixels / addlayer
pip install clipparse[all]    # 両方 — 編集時の CanvasPreview 再合成も有効になる
```

extras が無い環境でコマンドを叩くと、何を入れればよいか案内して終了する。

```
clip-to-psd in.clip out.psd  [--verify] [--flat]
psd-to-clip in.psd  out.clip [--verify] [--paper] [--template empty.clip]
```

- `--verify` は出力を読み直して入力と突き合わせる (CLIP→PSD はレイヤ画素の
  完全一致、PSD→CLIP は合成結果の一致)。
- CLIP→PSD はフォルダ構造 (通過は PSD の `pass` へ) と合成モード 27 種を
  写像する。`--flat` でフォルダなしの平坦化。**マスク・クリッピングは
  アルファに焼き込む** (見た目は合うが編集不可)。**調整レイヤ・ベクタレイヤは
  出力しない**。覆い焼き (発光) の半透明部は PSD に相当する α の扱いが無く
  非可逆。
- PSD→CLIP は同梱の雛形 (CSP で新規作成した空ファイル) からキャンバスごと
  組み立て直す。自前の雛形は `--template` で (寸法は作り替えるので任意)。
  `--paper` で雛形の白い用紙レイヤを残す。出力は CLIP STUDIO PAINT で
  開けることを実機 (PRO 5.0.4) で確認済み。

`tools/` のスクリプトは同じコードで、仕様の参照実装としてリポジトリに置いてある。

```
# 構造ダンプ (チャンク配置 / テーブル / Layer ツリー / ブロック列。標準ライブラリのみ)
python tools/clip_probe.py file.clip [--blocks]

# 遅延参照プロトタイプ: 合成して参照 PNG と比較する
python tools/clip_lazy_demo.py file.clip -o out.png --compare reference.png

# 書き出し (無変更往復はバイト一致するのが正)
python tools/clip_write.py roundtrip in.clip out.clip
python tools/clip_write.py set       in.clip out.clip --layer 5 --opacity 64 --composite 2
python tools/clip_write.py setpixels in.clip out.clip --layer 3 --png patch.png
python tools/clip_write.py addlayer  in.clip out.clip --copy-from 3 --name 追加 --png patch.png

# 参照整合性の検査。**CSP で開く前に必ず通す**
python tools/clip_validate.py out.clip

# レイヤ単位の診断と、壊れたレイヤ / マスク / サムネイルの除去
python tools/clip_doctor.py file.clip --deep
python tools/clip_doctor.py file.clip --fix --out fixed.clip

# CLIP <-> PSD 変換 (psdparse の Python バインディングが要る)
python tools/clip_to_psd.py input.clip output.psd  --verify
python tools/psd_to_clip.py input.psd  output.clip --verify
```

`clip_probe.py` は標準ライブラリのみ。`clip_lazy_demo.py` は numpy
(比較するときだけ Pillow) を使う。

## ビルド (C++ ライブラリ / CLI)

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

build\clipparse\Release\clip_cli.exe file.clip --check
build\clipparse\Release\clip_cli.exe file.clip --validate
build\clipparse\Release\clip_cli.exe in.clip --set 5 --opacity 64 --out out.clip
build\clipparse\Release\clip_cli.exe in.clip --set-pixels 3 rgba.raw out.clip
build\clipparse\Release\clip_cli.exe in.clip --add-layer  3 rgba.raw out.clip --name 追加
```

依存は **zlib と sqlite3 のみ**。どちらも CMake の `FetchContent` がソースから
取ってくるので vcpkg 等は不要。SQLite は
`sqlite3_deserialize(..., SQLITE_DESERIALIZE_READONLY)` で mmap 上を
そのまま参照する — 一時ファイルを作らない。

```cpp
clip::ClipFile f;
f.load("artwork.clip");
clip::Image img;
f.mergedImage(img);                       // RGBA8 ストレートアルファ

clip::ClipWriter w;
w.load("artwork.clip");
w.addLayer(3, "追加", rgba, 300, 400);
w.save("out.clip");
```

書く側は **C++ と Python の出力がチャンクのバイト列まで一致する** (zlib の出力まで同じ)。
これが正しさの担保で、テストが自動で確かめている。

## CLIP ⇄ PSD 変換

`examples/clipconv/` は clipparse と psdparse の**両方を参照する側**の独立した
コマンドで、どちらのライブラリの公開 API だけで書いてある。

```powershell
cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
cmake --build build-conv --config Release

build-conv\Release\clipconv.exe in.clip out.psd  --verify
build-conv\Release\clipconv.exe in.psd  out.clip --verify
```

レイヤの画素・フォルダのツリー・合成モードは往復しても保たれる。マスクと
クリッピングはアルファに焼き込まれ、調整レイヤ・ベクタレイヤは出力しない。
詳細は [examples/clipconv/README.md](examples/clipconv/README.md)。

## できること・できないこと

| | |
|---|---|
| 読み | 表現色 4 種 (RGBA / グレー / モノクロ / マスク)、フォルダ、マスク、クリッピング、テキスト、ラスタライズ済みベクタ |
| 合成 | 合成モード 27 種、通過フォルダ、調整レイヤ 5 種。**28 サンプル中 13 が CSP のプレビューと画素完全一致、22 が丸め誤差以内** |
| 書き | 属性編集・画素の差し替え・レイヤの追加削除・キャンバスの作り直し。すべて CSP 5.0.4 の実機で確認済み |
| 未対応 | ベクタレイヤ (ブラシエンジンが要る)、一部の調整レイヤ (レベル補正 / カラーバランス / ポスタリゼーション / グラデーションマップ) |

書く側には「**こちらのリーダでは読めるのに CSP が受け付けない**」種類の落とし穴が
いくつもある — テーブルごとに違う格納型、CSP が実際に照合しているチェックサム、
食い違うと CSP が落ちるミップ段数。CSP 実機で 5 巡かけて洗い出し、
`clipparse.validate()` / `clip_cli --validate` で機械的に検査できるようにしてある。
**書いたら開く前に必ず検査を通すこと。**

## テスト

```powershell
python -m pytest tests -q
```

テストは C++ 拡張と純 Python の参照実装を突き合わせ (画素がバイト一致すること)、
書き込み経路が CSP の作法を守っているかを見る。実ファイルの `.clip` は
コミットしていない。`samples/` に何を置くかは
[docs/STATUS.md](docs/STATUS.md) にある。

## 形式の要点 (5 行で)

```
[CSFCHUNK ヘッダ][CHNKHead][CHNKExta ...][CHNKSQLi][CHNKFoot]
                            ↑実データ      ↑全メタ情報 (SQLite3 DB)
```

`CHNKHead.binary_section_size` が `CHNKSQLi` の位置と一致するので、**先頭 64 バイト
だけ読めばメタ情報に直行できる**。そこから `ExternalChunk.Offset` と
`Offscreen.Attribute.BlockSize[]` の前置和だけで任意のピクセルブロックの
絶対位置が決まる — バイナリ領域を走査する必要がない。

## ドキュメント

| | |
|---|---|
| [docs/PYTHON_API.ja.md](docs/PYTHON_API.ja.md) ([en](docs/PYTHON_API.md)) | Python API リファレンス |
| [docs/CLIP_FORMAT.md](docs/CLIP_FORMAT.md) | `.clip` の形式仕様。**[実測]** と **[推定]** を区別して記述 |
| [docs/DESIGN.md](docs/DESIGN.md) | 遅延参照方式の設計、psdparse との共通 API、実装ロードマップ |
| [docs/STATUS.md](docs/STATUS.md) | 開発の到達点・再開手順・次にやること |
| [docs/CLIP_TOOLS_REPORT.md](docs/CLIP_TOOLS_REPORT.md) | clip-tools へのフィードバック (英語)。再現手順付きのバグ 3 件 + 仕様の訂正 2 件 |

## 一次資料

- [animeops/clip-tools](https://github.com/animeops/clip-tools) — 形式解析の
  出発点になった Python 実装。
- [psdparse](https://github.com/wamsoft/psdparse) — clipparse が倣った設計。

## ライセンス

MIT — [LICENSE](LICENSE) を参照。
