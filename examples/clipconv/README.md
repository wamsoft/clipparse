# clipconv — CLIP ⇄ PSD 変換コマンド (外部サンプル)

**clipparse と psdparse の両方を参照する側**のサンプル。どちらのライブラリも
このコマンドのために特別な口を持っていない — **公開 API だけ**で書いてある。
Python 版 (`tools/clip_to_psd.py` / `tools/psd_to_clip.py`) と同じことをする。

```powershell
cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
cmake --build build-conv --config Release

build-conv\Release\clipconv.exe in.clip out.psd  --verify
build-conv\Release\clipconv.exe in.psd  out.clip --verify
```

リポジトリを別の場所に置いている場合は 2 つのパスを渡す:

```powershell
cmake -S examples/clipconv -B build-conv `
      -DCLIPPARSE_DIR=D:/path/to/clipparse -DPSDPARSE_DIR=D:/path/to/psdparse
```

外部パッケージマネージャは要らない。zlib と sqlite3 は両ライブラリの
`FetchContent` がソースから取ってくる (zlib は両方が宣言するが、
FetchContent は同名を 1 回しか取得しないので実体はひとつ)。

## 使い方

```
clipconv in.clip out.psd  [--verify] [--flatten]
clipconv in.psd  out.clip [--template empty.clip] [--paper] [--verify]
```

方向はファイル名の拡張子で決まる。

| | |
|---|---|
| `--verify` | 書いた後で読み直し、元の画素と突き合わせる (差が出たら exit 1) |
| `--flatten` | CLIP → PSD でフォルダを作らず平坦化する |
| `--template` | PSD → CLIP の雛形 (既定 `samples/emptyimage.clip`) |
| `--paper` | 雛形の白い用紙レイヤを残す (既定は消す) |

**PSD → CLIP は空の `.clip` を雛形にする。** `Layer` 57 列 / `Canvas` 35 列の
うち CSP が期待する既定値の大半は意味が分かっていないので、ゼロから行を書くより
既存行を作り替える方が安全 (`docs/DESIGN.md` §6.1)。雛形は CSP で「新規」した
だけのファイルでよい。キャンバス寸法は `ClipWriter::resizeCanvas` が作り替える。

書いた後は `clip::validate` を必ず通している。**CSP で開く前にここを通すこと** —
ここで引っかかる種類の間違いは、寛容なリーダでは読めてしまうのに CSP では
落ちたり全面透明になったりする (`docs/CLIP_FORMAT.md`)。

## 動作確認

CLIP → PSD → CLIP の往復で、**13 サンプルすべて合成結果がバイト一致**
(ツリー・フォルダ・通過フォルダ・合成モードを保つ)。

Python 版との突き合わせ:

| | |
|---|---|
| `folder.clip` / `blend2.clip` → PSD | **sha256 一致** (Python 版と同じバイト列) |
| PSD → CLIP のチャンク | **ペイロードがバイト一致** |
| `text.clip` → PSD | **一致しない**。C++ 版はテキストを外接矩形 (31x151 等) で置き、Python 版はキャンバス全面で置く。C++ 版の方が小さい PSD になる |

速度 (`tama.clip` 60MB / 72 層):

| | C++ | Python |
|---|---|---|
| CLIP → PSD | **13.9 秒** | 2 分 11 秒 |
| PSD → CLIP | **45.6 秒** | 1 分 22 秒 |

## 制限

「完全でなくてよい」の範囲。Python 版と同じ:

1. **ラスタとフォルダのみ**。テキスト・ベクタ・調整レイヤは**ラスタ化**されて入る
2. マスクとクリッピングは**アルファに焼き込まれる** (編集可能な形では持ち込まない)
3. 覆い焼き(発光) (`LayerComposite = 10`) は PSD に対応が無く、
   半透明部が非可逆 (加算(発光) 12 は 11 と等価なので可逆)

**調整レイヤを持つファイルは見た目が往復しない。** 例えば `tama.clip` は
トーンカーブとカラーバランスを持つので、往復すると合成結果が大きく変わる
(Python 版も同じ)。レイヤの画素とツリーは保たれる。
