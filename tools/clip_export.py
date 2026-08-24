"""`.clip` から PNG を書き出す。**同梱の C++ 拡張だけで動く** (numpy / Pillow 不要)。

    clip-export file.clip                            # 合成結果を file.png へ
    clip-export file.clip -o out.png                 # 出力名を指定
    clip-export file.clip --list                     # レイヤ一覧 (index と MainId)
    clip-export file.clip --layer 5 -o layer.png     # レイヤ 1 枚 (--list の index)
    clip-export file.clip --layers outdir            # 画素を持つ全レイヤ + manifest.json

合成・画素展開は wheel に入っている C++ 拡張 (`clipparse`) が行い、
PNG の書き出しは zlib (標準ライブラリ) の最小実装で済ませる。
そのため pip 版では**追加の依存なしで**動く標準装備のコマンドになる。

レイヤ指定は読み取り API と同じ **index** (`ClipFile.layers` の並び。下から上、
中身がフォルダより先)。`clip-doctor` / `clip-probe` が表示する MainId とは別物
なので、`--list` か manifest.json で対応を確認すること。

依存: 標準ライブラリ + clipparse 拡張。リポジトリで拡張なしに実行したときは
案内を出して終了する (純 Python の合成は `clip_lazy_demo.py` の担当)。
"""

import argparse
import json
import os
import re
import struct
import sys
import zlib


def write_png(path, bgra, w, h):
    """BGRA バイト列 (ストレートアルファ) を RGBA8 PNG に書く。

    フィルタは全行 0。C++ 側の CanvasPreview 用ライタ (clipwrite.cpp) と
    同じ方針の最小実装で、圧縮率より依存の無さを取る。
    """
    if len(bgra) != w * h * 4:
        raise ValueError(f"pixel buffer {len(bgra)} != {w}x{h}x4")
    rgba = bytearray(bgra)
    rgba[0::4] = bgra[2::4]                 # B <-> R (スライス代入なので C 速度)
    rgba[2::4] = bgra[0::4]
    stride = w * 4
    raw = b"".join(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride])
                   for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    out = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def _backend():
    """C++ 拡張を返す。無ければ案内して None。"""
    try:
        import clipparse
    except ImportError:
        clipparse = None
    if clipparse is None or not hasattr(clipparse, "ClipFile"):
        print("clipparse の C++ 拡張が見つからない (pip install clipparse で入る)。\n"
              "リポジトリで使うときはビルドしてから:\n"
              "  cmake -S . -B build-py -DCLIPPARSE_BUILD_PYTHON=ON && "
              "cmake --build build-py --config Release\n"
              "  $env:PYTHONPATH=\"$PWD\\build-py\\python\\Release\"")
        return None
    return clipparse


def _open(cp, path):
    f = cp.ClipFile()
    if not f.load(path):
        print(f"読めない: {path} ({f.error})")
        return None
    return f


_BAD = re.compile(r'[\x00-\x1f\\/:*?"<>|]+')


def _safe_name(name):
    return _BAD.sub("_", name).strip() or "layer"


def _list(f):
    for i in f.roots[::-1]:
        _dump_tree(f, i, 0)


def _dump_tree(f, i, depth):
    r = f.layers[i]
    kind = ("folder" if r.is_group else
            "filter" if r.is_filter else
            "text" if r.is_text else "layer")
    box = f"{r.width}x{r.height}@({r.left},{r.top})" if r.width else "-"
    print(f"  {'  ' * depth}[{r.index:>3}] #{r.main_id} {r.name!r} {kind} "
          f"vis={int(r.visible)} opa={r.opacity_raw}/256 {box}")
    for k in r.children[::-1]:
        _dump_tree(f, k, depth + 1)


def _export_layer(f, r, path, mode):
    if r.is_group:
        print(f"  [{r.index}] {r.name!r} はフォルダ (画素なし)")
        return None
    if r.width <= 0 or r.height <= 0:
        print(f"  [{r.index}] {r.name!r} は画素なし (bbox が空)")
        return None
    n = write_png(path, f.layer_image(r.index, mode), r.width, r.height)
    print(f"  [{r.index}] #{r.main_id} {r.name!r} {r.width}x{r.height} "
          f"-> {path}  {n:,} B")
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("-o", "--out", help="出力 PNG (既定: 入力名.png)")
    ap.add_argument("--list", action="store_true", help="レイヤ一覧を出して終わる")
    ap.add_argument("--layer", type=int, metavar="INDEX",
                    help="このレイヤ 1 枚だけ書き出す (--list の index)")
    ap.add_argument("--layers", metavar="DIR",
                    help="画素を持つ全レイヤを DIR へ書き出す (+ manifest.json)")
    ap.add_argument("--mode", default="masked", choices=("masked", "raw"),
                    help="レイヤ画素にマスクを適用するか (既定 masked)")
    ap.add_argument("--hidden", action="store_true",
                    help="--layers で非表示レイヤも書き出す")
    args = ap.parse_args(argv)

    cp = _backend()
    if cp is None:
        return 2
    f = _open(cp, args.src)
    if f is None:
        return 1

    if args.list:
        print(f"{args.src}  {f.width}x{f.height}  レイヤ {len(f.layers)}")
        _list(f)
        return 0

    if args.layer is not None:
        if not 0 <= args.layer < len(f.layers):
            print(f"index {args.layer} が範囲外 (0..{len(f.layers) - 1}。--list で確認)")
            return 1
        r = f.layers[args.layer]
        out = args.out or f"{_safe_name(r.name)}.png"
        return 0 if _export_layer(f, r, out, args.mode) else 1

    if args.layers:
        os.makedirs(args.layers, exist_ok=True)
        manifest = {"source": os.path.basename(args.src),
                    "canvas": {"width": f.width, "height": f.height},
                    "mode": args.mode, "layers": []}
        n = 0
        for r in f.layers:
            if r.is_group or r.width <= 0 or r.height <= 0:
                continue
            if not r.visible and not args.hidden:
                continue
            name = f"{r.index:03d}_{_safe_name(r.name)}.png"
            if _export_layer(f, r, os.path.join(args.layers, name), args.mode):
                manifest["layers"].append({
                    "file": name, "index": r.index, "main_id": r.main_id,
                    "name": r.name, "visible": r.visible,
                    "opacity": r.opacity_raw, "composite": r.composite_raw,
                    "clipping": r.clipping, "left": r.left, "top": r.top,
                    "width": r.width, "height": r.height,
                    "parent_index": r.parent_index,
                })
                n += 1
        mpath = os.path.join(args.layers, "manifest.json")
        with open(mpath, "w", encoding="utf-8") as fp:
            json.dump(manifest, fp, ensure_ascii=False, indent=2)
        print(f"  {n} レイヤ + manifest.json -> {args.layers}")
        return 0 if n else 1

    out = args.out or os.path.splitext(args.src)[0] + ".png"
    n = write_png(out, f.merged_image(), f.width, f.height)
    print(f"  合成 {f.width}x{f.height} -> {out}  {n:,} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
