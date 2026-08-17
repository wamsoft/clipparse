"""PSD -> CLIP 変換 (W4)。

    python tools/psd_to_clip.py in.psd out.clip
    python tools/psd_to_clip.py in.psd out.clip --paper --verify

`clip_to_psd.py` の逆方向。**空の `.clip` を雛形にして組み立てる**:

1. `samples/emptyimage.clip` (CSP で「新規」しただけのファイル) を開く
2. `clip_build.resize_canvas` でキャンバスを PSD の寸法へ作り替える
3. PSD のレイヤツリーを**下から順に** `clip_write.add_layer` で積む
4. 雛形の 用紙 / レイヤー 1 を消す

**なぜ雛形か**: `Layer` 57 列 / `Canvas` 35 列のうち CSP が期待する既定値の
大半は意味が分かっていない。ゼロから書くより既存行を作り替える方が安全。

**制限** (「完全でなくてよい」の範囲):

- ラスタと**フォルダ**のみ。テキスト・ベクタ・調整レイヤは**ラスタ化して**入る
  (PSD 側が持っている合成済み画素をそのまま置く)
- レイヤマスクは PSD の画素に焼き込まれた状態で入る (マスクとしては持たない)
- 合成モードは PSD の `BlendMode` -> `LayerComposite` へ写像。対応の無いものは通常
- **CSP 実機での確認は未了** (docs/WRITE_TEST_3.md)

依存: psdparse (Python バインディング) / numpy / Pillow。
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import psdparse                                            # noqa: E402
from clip_build import resize_canvas                       # noqa: E402
from clip_write import (ClipFile, add_layer, delete_layer,  # noqa: E402
                        refresh_preview)

DEFAULT_TEMPLATE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "samples", "emptyimage.clip")

# clip_to_psd.BLEND_TO_PSD の逆。1 対多だった 10/12 は情報を保つ側 (9/11) を選ぶ。
#
# 注意: `LayerInfo.blend_mode_key` は **4 文字コードを詰めた int** を返す。
# 文字列と思って比較すると全部「通常」に落ちる。列挙型の `blend_mode` を使う。
PSD_TO_BLEND = {
    "NORMAL": 0, "DARKEN": 1, "MULTIPLY": 2, "COLOR_BURN": 3, "LINEAR_BURN": 4,
    "SUBTRACT": 5, "DARKER_COLOR": 6, "LIGHTEN": 7, "SCREEN": 8,
    "COLOR_DODGE": 9, "LINEAR_DODGE": 11, "LIGHTER_COLOR": 13, "OVERLAY": 14,
    "SOFT_LIGHT": 15, "HARD_LIGHT": 16, "VIVID_LIGHT": 17, "LINEAR_LIGHT": 18,
    "PIN_LIGHT": 19, "HARD_MIX": 20, "DIFFERENCE": 21, "EXCLUSION": 22,
    "HUE": 23, "SATURATION": 24, "COLOR": 25, "LUMINOSITY": 26,
    "PASS_THROUGH": 30, "DIVIDE": 36,
}

FOLDER_BIT = 1
FOLDER_CLOSED_BIT = 1 << 4


def _key(layer):
    return layer.blend_mode.name


def _canvas_rgba(psd, index, W, H):
    """PSD レイヤの画素をキャンバス全面の RGBA へ置く。

    CLIP の 100% ミップは**常にキャンバス全面**なので、PSD の矩形を
    そこへ貼り込む (はみ出しは切る)。
    """
    lay = psd.layers[index]
    out = np.zeros((H, W, 4), np.uint8)
    if lay.width <= 0 or lay.height <= 0:
        return out
    data = psd.layer_image(index)
    if not data:
        return out
    # psdparse も CLIP と同じ **BGRA** 並びで返す。CLIP へ書く側
    # (clip_encode.encode_rgba_block) は RGBA を取るのでここで入れ替える。
    src = np.frombuffer(data, np.uint8).reshape(
        lay.height, lay.width, 4)[..., [2, 1, 0, 3]]

    x0, y0 = max(0, lay.left), max(0, lay.top)
    x1, y1 = min(W, lay.left + lay.width), min(H, lay.top + lay.height)
    if x0 >= x1 or y0 >= y1:
        return out
    out[y0:y1, x0:x1] = src[y0 - lay.top:y1 - lay.top, x0 - lay.left:x1 - lay.left]
    return out


def build(psd, clip, indices, parent, W, H, template_layer, checksum, log,
          depth=0):
    """PSD のツリーを下から順に CLIP へ積む。追加した枚数を返す。

    psdparse の `roots` / `children` は**下が先**の平坦順に並んでいて、
    CLIP の子チェーンも**先頭が最下層**。順序はそのまま使える。
    """
    n = 0
    for i in indices:
        lay = psd.layers[i]
        name = lay.name_unicode or lay.name or ""
        # PSD 0..255 -> CLIP 0..256。段数が 256 対 257 なので厳密には
        # 1 対 1 にならない。`clip_to_psd` の切り捨て (`c*255//256`) に対して
        # **切り上げ**を返すと c=1 以外は往復で元に戻る [実測]。
        opa = min(256, -(-lay.opacity * 256 // 255))
        comp = PSD_TO_BLEND.get(_key(lay), 0)
        over = {
            "LayerOpacity": opa,
            "LayerVisibility": 1 if lay.visible else 0,
            "LayerComposite": comp,
            "LayerClip": 1 if lay.clipping else 0,
        }
        pad = "  " * depth

        if lay.is_group:
            over.update({"LayerType": 0, "LayerFolder": FOLDER_BIT})
            fid = add_layer(clip, template_layer, name, None, parent=parent,
                            checksum=checksum, overrides=over)
            log.append(f"    {pad}[F] {name!r}  comp={comp} opa={opa}")
            n += 1
            n += build(psd, clip, lay.children, fid, W, H, template_layer,
                       checksum, log, depth + 1)
            continue

        if lay.layer_type == psdparse.LayerType.HIDDEN:
            continue                                  # 区切りレイヤは持ち込まない
        rgba = _canvas_rgba(psd, i, W, H)
        add_layer(clip, template_layer, name, rgba, parent=parent,
                  checksum=checksum, overrides=over)
        px = int((rgba[..., 3] > 0).sum())
        log.append(f"    {pad}{name!r}  comp={comp} opa={opa} 画素={px:,}")
        n += 1
    return n


def convert(src, dst, template=DEFAULT_TEMPLATE, paper=False, checksum="zero",
            verbose=True):
    psd = psdparse.PSDFile()
    if not psd.load(src):
        raise RuntimeError(f"PSD を読めない: {src}")
    W, H = psd.header.width, psd.header.height

    clip = ClipFile(template)
    cur = clip.db.cursor()
    root = cur.execute("SELECT CanvasRootFolder FROM Canvas").fetchone()[0]

    # 雛形の中身を把握する。ラスタ 1 枚を**複製元として最後まで残す**
    kids = []
    node = cur.execute("SELECT LayerFirstChildIndex FROM Layer WHERE MainId=?",
                       (root,)).fetchone()[0]
    while node:
        mid, folder = cur.execute(
            "SELECT MainId, LayerFolder FROM Layer WHERE MainId=?",
            (node,)).fetchone()
        kids.append((mid, folder))
        node = cur.execute("SELECT LayerNextIndex FROM Layer WHERE MainId=?",
                           (node,)).fetchone()[0]
    rasters = [m for m, f in kids if not (f & FOLDER_BIT)]
    if len(rasters) < 2:
        raise RuntimeError("雛形にラスタレイヤが 2 枚 (用紙 + 1) 必要")
    template_layer = rasters[-1]                     # いちばん上 = レイヤー 1
    paper_layer = rasters[0]

    resize_canvas(clip.db, W, H, dpi=psd.header.hres or None)
    clip.externals = []                              # 実体は全部作り直す

    log = [f"  {src}  {W}x{H}  レイヤ {len(psd.layers)}"]
    n = build(psd, clip, psd.roots, root, W, H, template_layer, checksum, log)

    if not paper:
        delete_layer(clip, paper_layer)
    delete_layer(clip, template_layer)


    size = clip.save(dst)
    clip.close()

    # CSP は開いた直後 `CanvasPreview` を表示する。雛形のものを残すと
    # 「起動直後だけ雛形の白いキャンバスが出る」ことになる [実測: WRITE_TEST_4 ④]。
    # **PSD の merged image は当てにしない** (Photoshop 以外が作った PSD では
    # 空のことがある)。書いた `.clip` を自前で合成し直すのが確実。
    size = refresh_preview(dst)
    log.append(f"  {dst}  {size:,} B  ({n} レイヤ, 用紙={'あり' if paper else 'なし'})")
    if verbose:
        print("\n".join(log))
    return dst


def verify(psd_path, clip_path):
    """書いた CLIP を読み直して PSD と突き合わせる。"""
    import imgdoc
    psd = psdparse.PSDFile()
    psd.load(psd_path)
    d = imgdoc.open(clip_path)
    W, H = psd.header.width, psd.header.height
    ok = True

    if (d.header.width, d.header.height) != (W, H):
        print(f"  NG キャンバス {d.header.width}x{d.header.height} != {W}x{H}")
        return False

    def flat(doc, idx, out):
        for i in idx:
            lay = doc.layers[i]
            if lay.is_group:
                out.append(("F", lay.name_unicode))
                flat(doc, lay.children, out)
            elif lay.layer_type != psdparse.LayerType.HIDDEN:
                out.append(("L", lay.name_unicode))
        return out

    a, b = flat(psd, psd.roots, []), flat(d, d.roots, [])
    if a != b:
        print(f"  NG ツリーが違う\n    PSD : {a}\n    CLIP: {b}")
        ok = False

    worst = 0
    ci = {i: l for i, l in enumerate(d.layers)}
    for i, lay in enumerate(psd.layers):
        if lay.is_group or lay.layer_type == psdparse.LayerType.HIDDEN:
            continue
        want = _canvas_rgba(psd, i, W, H)
        j = next((k for k, l in ci.items()
                  if l.name_unicode == (lay.name_unicode or lay.name)
                  and not l.is_group), None)
        if j is None:
            print(f"  NG {lay.name_unicode!r} が CLIP に無い")
            ok = False
            continue
        got = np.frombuffer(d.layer_image(j), np.uint8).reshape(
            d.layers[j].height, d.layers[j].width, 4)[..., [2, 1, 0, 3]]
        # α=0 の画素の RGB は意味を持たない。PSD は白を残すが、CLIP は
        # 全面透明なブロックを**空レコード**にするので 0 で読み戻る。
        vis = (want[..., 3] > 0) | (got[..., 3] > 0)
        dv = np.abs(got.astype(int) - want.astype(int))
        dv[..., :3] *= vis[..., None]
        diff = int(dv.max())
        worst = max(worst, diff)
        del ci[j]
    print(f"  画素の最大差 max={worst}  ツリー={'一致' if a == b else '不一致'}")
    return ok and worst == 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="PSD -> CLIP")
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--template", default=DEFAULT_TEMPLATE,
                    help="雛形の空 .clip (既定: samples/emptyimage.clip)")
    ap.add_argument("--paper", action="store_true",
                    help="雛形の白い用紙レイヤを残す (既定は消す)")
    ap.add_argument("--checksum", default="zero", choices=("zero", "crc32", "none"))
    ap.add_argument("--verify", action="store_true",
                    help="書いた後で読み直して PSD と突き合わせる")
    args = ap.parse_args(argv)

    convert(args.src, args.dst, args.template, args.paper, args.checksum)
    if args.verify:
        return 0 if verify(args.src, args.dst) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
