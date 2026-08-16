"""CLIP → PSD 変換 (完全変換ではない、実用重視の版)。

    python tools/clip_to_psd.py input.clip output.psd [--verify] [--flat]

方針:

- **レイヤフォルダを保つ** (psdparse の `add_folder` が要る)。
  無い版の psdparse では自動的に平坦化へ落ちる。`--flat` で明示的に平坦化。
- **マスクとクリッピングはアルファに焼き込む**。見た目は合うが、
  PSD 側で編集可能な形では残らない。
- 合成モードは `LayerComposite` → PSD の 4 文字キーへ写像する (下表)。
  CSP 固有で PSD に無いものは近いモードへ倒す。
- 調整レイヤ・ベクタレイヤは出力しない (ラスタが無いため)。
- 合成結果を PSD のプレビュー (merged image) に書き込むので、
  Photoshop で開く前でもサムネイルが正しく出る。

依存: numpy / psdparse (Python バインディング)。
"""

import argparse
import importlib.util
import os
import sys

import numpy as np

_spec = importlib.util.spec_from_file_location(
    "clip_lazy_demo", os.path.join(os.path.dirname(__file__), "clip_lazy_demo.py"))
demo = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(demo)


# CLIP の LayerComposite → PSD の 4 文字ブレンドキー。
# 実測で式を同定した対応 (docs/CLIP_FORMAT.md §9)。
# † = PSD に厳密な相当が無く、近いモードへ倒しているもの。
BLEND_TO_PSD = {
    0:  "norm",              # 通常
    1:  "dark",              # 比較 (暗)
    2:  "mul ",              # 乗算
    3:  "idiv",              # 焼きこみカラー
    4:  "lbrn",              # 焼きこみ (リニア)
    5:  "fsub",              # 減算
    6:  "dkCl",              # カラー比較 (暗)
    7:  "lite",              # 比較 (明)
    8:  "scrn",              # スクリーン
    9:  "div ",              # 覆い焼きカラー
    10: "div ",              # 覆い焼き (発光) † α の扱いが違う (半透明で差が出る)
    11: "lddg",              # 加算
    12: "lddg",              # 加算 (発光)  … 11 と等価なので可逆
    13: "lgCl",              # カラー比較 (明)
    14: "over",              # オーバーレイ
    15: "sLit",              # ソフトライト
    16: "hLit",              # ハードライト
    17: "vLit",              # ビビッドライト
    18: "lLit",              # リニアライト
    19: "pLit",              # ピンライト
    20: "hMix",              # ハードミックス
    21: "diff",              # 差の絶対値
    22: "smud",              # 除外
    23: "hue ",              # 色相
    24: "sat ",              # 彩度
    25: "colr",              # カラー
    26: "lum ",              # 輝度
    30: "pass",              # 通過 (フォルダ専用。平坦化するので実際には出ない)
    36: "fdiv",              # 除算
}

# 半透明部分があると PSD 側で再現できないモード
LOSSY_WITH_ALPHA = {10}


FOLDER_CLOSED_BIT = 1 << 4       # Layer.LayerFolder


def build(clip, psd, W, H, parent_id, log, use_folders):
    """CLIP のレイヤツリーを下から順に PSD へ積む。追加した枚数を返す。

    フォルダは中身を積んでから `add_folder` で包む。psdparse は
    layerList の index 0 が最下層なので、CLIP の子チェーン順とそのまま合う。
    """
    rows = {r[0]: r for r in clip.cur.execute(
        "SELECT MainId, LayerName, LayerFirstChildIndex, LayerNextIndex, LayerVisibility,"
        " LayerOpacity, LayerComposite, LayerFolder, LayerType, LayerClip FROM Layer")}
    added = 0
    clip_base = None
    child = rows[parent_id][2]
    while child:
        (mid, name, _fc, nxt, vis, opa, comp, folder, ltype, lclip) = rows[child]
        child = nxt
        key = BLEND_TO_PSD.get(comp, "norm")
        ps_opa = min(255, opa * 255 // 256)      # CLIP は 0..256、PSD は 0..255

        if folder & 1:
            start = len(psd.layers)
            inner = build(clip, psd, W, H, mid, log, use_folders)
            if use_folders:
                fidx = psd.add_folder(name or "folder", start, inner,
                                      bool(folder & FOLDER_CLOSED_BIT), key, ps_opa)
                if not vis:
                    psd.layers[fidx].visible = False
                log.append(f"  folder {name!r}: {inner} 枚を包む "
                           f"blend={comp}->{key!r} opacity={ps_opa}"
                           f"{' 折り畳み' if folder & FOLDER_CLOSED_BIT else ''}"
                           f"{' 非表示' if not vis else ''}")
                added += inner + 2
            else:
                added += inner
            clip_base = None
            continue

        if ltype & demo.FILTER_BIT:
            log.append(f"  skip   {name!r}: 調整レイヤ (未対応)")
            continue

        demo.VERBOSE = False
        img = demo.layer_pixels(clip, mid, W, H, 0, "")
        if img is None:
            log.append(f"  skip   {name!r}: ラスタ無し (ベクタ等)")
            continue

        note = []
        if ltype & 2:                            # レイヤマスクを焼き込む
            moff = clip.top_offscreen(mid, mask=True)
            if moff is not None and clip.has_pixels(moff):
                m = demo.fit_canvas(clip.offscreen_image(moff), W, H)
                img[..., 3] = (img[..., 3].astype(np.uint32)
                               * m[..., 3] // 255).astype(np.uint8)
                note.append("マスクをαへ焼き込み")

        if lclip and clip_base is not None:      # クリッピングも焼き込む
            img[..., 3] = ((img[..., 3].astype(np.uint32)
                            * clip_base + 127) // 255).astype(np.uint8)
            note.append("クリッピングをαへ焼き込み")
        elif not lclip:
            clip_base = img[..., 3].copy()

        alpha = img[..., 3]
        if comp in LOSSY_WITH_ALPHA and (alpha > 0).any() and (alpha < 255).any():
            note.append("発光モード: 半透明部が非可逆")

        idx = psd.add_layer(name or "layer", 0, 0,
                            img[..., [2, 1, 0, 3]].tobytes(), W, H, key, ps_opa)
        if not vis:
            psd.layers[idx].visible = False
        added += 1
        log.append(f"  add    {name!r}: blend={comp}->{key!r} opacity={opa}/256->{ps_opa}"
                   f"{' 非表示' if not vis else ''}"
                   f"{'  (' + ' / '.join(note) + ')' if note else ''}")
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--verify", action="store_true",
                    help="書き出した PSD を読み直して画素が一致するか確かめる")
    ap.add_argument("--flat", action="store_true",
                    help="フォルダを作らず平坦化する")
    args = ap.parse_args()

    import psdparse

    use_folders = hasattr(psdparse.PSDFile, "add_folder") and not args.flat
    if not use_folders and not args.flat:
        print("  注意: この psdparse に add_folder が無いので平坦化します "
              "(psdparse を再ビルド/再インストールすると保たれます)")

    clip = demo.ClipFile(args.src)
    W, H, root = clip.canvas()
    print(f"{args.src}: canvas {W}x{H}  フォルダ保持={'はい' if use_folders else 'いいえ'}")

    psd = psdparse.PSDFile()
    psd.create_blank(W, H)
    log = []
    build(clip, psd, W, H, root, log, use_folders)
    for line in log:
        print(line)

    # 合成結果をプレビューに入れる (Photoshop が再合成するまでの見た目)
    demo.VERBOSE = False
    composite = demo.composite(clip, root, W, H, 0)
    psd.set_merged_image(composite[..., [2, 1, 0, 3]].tobytes())

    if not psd.save(args.dst):
        print("  save 失敗")
        return 1
    nfolder = sum(1 for l in psd.layers if str(l.layer_type).endswith("FOLDER"))
    print(f"  -> {args.dst}  ({os.path.getsize(args.dst):,} B, "
          f"{len(psd.layers)} 層 / フォルダ {nfolder})")

    if args.verify:
        chk = psdparse.PSDFile()
        chk.load(args.dst)
        bad = same = 0
        for i, l in enumerate(chk.layers):
            if l.width <= 0 or l.height <= 0:
                continue                          # フォルダ/区切りは画素なし
            got = np.frombuffer(chk.layer_image(i, "image"), np.uint8)
            src = np.frombuffer(psd.layer_image(i, "image"), np.uint8)
            if got.shape == src.shape and (got == src).all():
                same += 1
            else:
                bad += 1
                print(f"    NG [{i}] {l.name_unicode!r}")
        print(f"  画素検証: {same}/{same + bad} レイヤが完全一致")
        print(f"  ツリー検証: 読み直し {len(chk.layers)} 層 / "
              f"フォルダ {sum(1 for l in chk.layers if str(l.layer_type).endswith('FOLDER'))}")
        return 0 if bad == 0 else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
