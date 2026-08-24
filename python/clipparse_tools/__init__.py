"""clipparse に同梱される検証・修復・変換ツール群。

ソースの正本はリポジトリの `tools/` (仕様検証の参照実装)。wheel のビルド時に
ここへ取り込まれる (`python/CMakeLists.txt`)。コンソールコマンドも同じもの:

    clip-probe    FILE.clip            構造ダンプ (チャンク / テーブル / ツリー / ブロック)
    clip-validate FILE.clip            参照整合性の検査 (CSP で開く前に通す)
    clip-doctor   FILE.clip            レイヤ単位の診断と不正部分の除去
    clip-export   FILE.clip            合成 / レイヤの PNG 書き出し (同梱拡張で完結)
    clip-write    roundtrip IN OUT     書き出し (往復 / 属性編集 / 画素差し替え / レイヤ追加)
    clip-to-psd   IN.clip OUT.psd      CLIP -> PSD 変換   [要 pip install clipparse[psd]]
    psd-to-clip   IN.psd  OUT.clip     PSD -> CLIP 変換   [要 pip install clipparse[psd]]

上 5 つは追加の依存なしで動く (clip-write の setpixels / addlayer だけ
`clipparse[image]` = numpy + Pillow が要る)。変換 2 つは `clipparse[psd]` を
入れた環境でだけ有効で、無いときは案内を出して終了する。
CanvasPreview の再合成は `clipparse[all]` 相当 (psd + image) が要る。
"""

import importlib


def _run(module, extra):
    """extras が無い環境では ImportError を握って案内を出す。"""
    try:
        mod = importlib.import_module(f".{module}", __package__)
    except ImportError as e:
        print(f"この機能には追加ライブラリが必要です:\n"
              f"    pip install clipparse[{extra}]\n"
              f"(足りないもの: {e})")
        return 2
    return mod.main()


def clip_to_psd_main():
    return _run("clip_to_psd", "psd")


def psd_to_clip_main():
    return _run("psd_to_clip", "psd")
