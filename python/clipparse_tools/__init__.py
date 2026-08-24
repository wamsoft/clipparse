"""clipparse に同梱される検証・修復ツール群 (標準ライブラリのみで動く)。

ソースの正本はリポジトリの `tools/` (仕様検証の参照実装)。wheel のビルド時に
ここへ取り込まれる (`python/CMakeLists.txt`)。コンソールコマンドも同じもの:

    clip-probe    FILE.clip            構造ダンプ (チャンク / テーブル / ツリー / ブロック)
    clip-validate FILE.clip            参照整合性の検査 (CSP で開く前に通す)
    clip-doctor   FILE.clip            レイヤ単位の診断と不正部分の除去
    clip-write    roundtrip IN OUT     書き出し (往復 / 属性編集 / 画素差し替え / レイヤ追加)

`clip_encode` / `clip_build` は `clip-write setpixels` / `addlayer` が使う補助で、
numpy / Pillow がある環境でだけ動く (import は遅延されている)。
CanvasPreview の再合成だけはリポジトリ環境 (imgdoc) が要るので、pip 版では
警告を出してスキップする。
"""
