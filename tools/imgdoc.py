"""psdparse 互換の読み取り API を .clip に被せる層。

    import imgdoc
    doc = imgdoc.open("file.clip")     # .psd なら psdparse.PSDFile をそのまま返す

    doc.header.width / height
    doc.layers          -> list[Layer]   平坦・下から上
    doc.roots           -> list[int]     最上位 (ツリービュー)
    doc.children(i)     -> list[int]
    doc.merged_image()  -> bytes         BGRA
    doc.layer_image(i, mode) -> bytes    BGRA

`docs/DESIGN.md` §5 の共通面をそのまま実装したもの。psdparse の
`examples/` や `tools/` を **無改造で .clip に対して動かす**のが目的で、
インタフェース設計の検証も兼ねている。

なぜクラス階層でなく duck typing か: 利用側は `import psdparse` して
`psdparse.LayerType.NORMAL` と比較する。共通の基底クラスを挟むより、
**psdparse の enum をそのまま返す**方が既存コードが素直に動く。

**バックエンドは 2 つある。** C++ 拡張 (`clipparse`) があればそちらを、
無ければ純 Python の参照実装 (`clip_lazy_demo`) を使う。どちらでも結果は
同じ (回帰で画素バイト一致を確認している)。現在の選択は `imgdoc.BACKEND`。

依存: numpy / psdparse (enum を借りる) / clipparse または clip_lazy_demo。
"""

import importlib.util
import os

import numpy as np
import psdparse

try:
    import clipparse as _cpp
    # リポジトリ直下の `clipparse/` (C++ ソース) が namespace package として
    # 拾われることがあるので、中身があるかまで確かめる。
    if not hasattr(_cpp, "ClipFile"):
        _cpp = None
except ImportError:                                  # 拡張が無ければ純 Python
    _cpp = None

_spec = importlib.util.spec_from_file_location(
    "clip_lazy_demo", os.path.join(os.path.dirname(__file__), "clip_lazy_demo.py"))
_demo = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_demo)
_demo.VERBOSE = False

BACKEND = "cpp" if _cpp is not None else "python"


# CLIP の LayerComposite → PSD の 4 文字キー (docs/CLIP_FORMAT.md §9 で実測同定)
BLEND_TO_PSD = {
    0: "norm", 1: "dark", 2: "mul ", 3: "idiv", 4: "lbrn", 5: "fsub", 6: "dkCl",
    7: "lite", 8: "scrn", 9: "div ", 10: "div ", 11: "lddg", 12: "lddg",
    13: "lgCl", 14: "over", 15: "sLit", 16: "hLit", 17: "vLit", 18: "lLit",
    19: "pLit", 20: "hMix", 21: "diff", 22: "smud", 23: "hue ", 24: "sat ",
    25: "colr", 26: "lum ", 30: "pass", 36: "fdiv",
}

_PSD_BLEND_NAME = {
    "norm": "NORMAL", "dark": "DARKEN", "mul ": "MULTIPLY", "idiv": "COLOR_BURN",
    "lbrn": "LINEAR_BURN", "fsub": "SUBTRACT", "dkCl": "DARKER_COLOR",
    "lite": "LIGHTEN", "scrn": "SCREEN", "div ": "COLOR_DODGE",
    "lddg": "LINEAR_DODGE", "lgCl": "LIGHTER_COLOR", "over": "OVERLAY",
    "sLit": "SOFT_LIGHT", "hLit": "HARD_LIGHT", "vLit": "VIVID_LIGHT",
    "lLit": "LINEAR_LIGHT", "pLit": "PIN_LIGHT", "hMix": "HARD_MIX",
    "diff": "DIFFERENCE", "smud": "EXCLUSION", "hue ": "HUE", "sat ": "SATURATION",
    "colr": "COLOR", "lum ": "LUMINOSITY", "pass": "PASS_THROUGH", "fdiv": "DIVIDE",
}

FILTER_BIT = 4096
FOLDER_BIT = 1


def _key_to_int(key):
    return sum(ord(c) << s for c, s in zip(key, (24, 16, 8, 0)))


def _blend_of(comp):
    key = BLEND_TO_PSD.get(comp, "norm")
    return getattr(psdparse.BlendMode, _PSD_BLEND_NAME.get(key, "NORMAL"))


class Header:
    """psdparse.Header 互換。CLIP は常に 8bit RGB として見せる。"""

    def __init__(self, width, height, resolution):
        self.width = width
        self.height = height
        self.channels = 4
        self.depth = 8
        self.mode = psdparse.COLOR_MODE_RGB
        self.version = 1
        self.hres = self.vres = resolution


class Channel:
    """psdparse.ChannelInfo 互換。CLIP のプレーン構成を PSD の id で見せる。"""

    __slots__ = ("id", "length")

    def __init__(self, cid, length):
        self.id = cid
        self.length = length

    def __repr__(self):
        return f"<Channel id={self.id} length={self.length}>"


class Layer:
    """psdparse.LayerInfo 互換 (読み取りのみ)。"""

    def __init__(self, doc, index, main_id, name, visible, opacity_raw, composite,
                 clipping, is_group, is_filter, is_text, bounds, channels):
        self._doc = doc
        self._index = index
        self.main_id = main_id
        self.layer_id = main_id
        self.name = self.name_unicode = name or ""
        self.visible = bool(visible)
        self.opacity_raw = opacity_raw               # CLIP 固有 (0..256)
        self.opacity = min(255, opacity_raw * 255 // 256)
        self.composite_raw = composite               # CLIP 固有
        self.blend_mode_key = _key_to_int(BLEND_TO_PSD.get(composite, "norm"))
        self.blend_mode = _blend_of(composite)
        self.clipping = 1 if clipping else 0
        self.is_group = is_group
        self.channels = channels
        self.transparency_protected = False
        self.obsolete = False
        self.pixel_data_irrelevant = False
        self.fill_opacity = 255

        if is_group:
            self.layer_type = psdparse.LayerType.FOLDER
        elif is_filter:
            self.layer_type = psdparse.LayerType.ADJUST
        elif is_text:
            self.layer_type = psdparse.LayerType.TEXT
        else:
            self.layer_type = psdparse.LayerType.NORMAL

        self.left, self.top, self.width, self.height = bounds
        self.right = self.left + self.width
        self.bottom = self.top + self.height

    @property
    def parent_index(self):
        return self._doc._parent.get(self._index, -1)

    @property
    def children(self):
        return self._doc.children(self._index)

    def __repr__(self):
        return (f"<Layer {self._index} {self.name_unicode!r} "
                f"{self.width}x{self.height} {self.layer_type}>")


class ClipDocument:
    """psdparse.PSDFile 互換の読み取り面を .clip に被せたもの。"""

    def __init__(self, path, backend=None):
        self.backend = backend or BACKEND
        if self.backend == "cpp" and _cpp is None:
            raise RuntimeError("clipparse 拡張が見つからない")
        self.merged_alpha = True
        self.is_loaded = True
        self._image_cache = {}
        self._composite = None
        if self.backend == "cpp":
            self._init_cpp(path)
        else:
            self._init_python(path)

    # --- C++ バックエンド -----------------------------------------------

    def _init_cpp(self, path):
        c = _cpp.ClipFile()
        if not c.load(path):
            raise OSError(f"cannot load {path}: {c.error}")
        self._clip = c
        self.header = Header(c.width, c.height, c.resolution)
        self._parent = {}
        self._children = {}
        self.layers = []
        for i, l in enumerate(c.layers):
            self._parent[i] = l.parent_index
            self._children[i] = list(l.children)
            self.layers.append(Layer(
                self, i, l.main_id, l.name_unicode, l.visible, l.opacity_raw,
                l.composite_raw, l.clipping, l.is_group, l.is_filter, l.is_text,
                (l.left, l.top, l.width, l.height), self._channels_cpp(l)))
        self._children[-1] = list(c.roots)

    def _channels_cpp(self, l):
        if l.is_group:
            return []
        off = self._clip.top_offscreen(l.main_id)
        a = self._clip.attribute(off) if off else None
        if a is None:
            return []
        size = a.plane_bytes or (a.block_width * a.block_height)
        n = a.num_channels
        ids = [-1, 0, 1, 2] if n == 4 else ([-1, 0] if n == 1 else [-1])
        return [Channel(i, size) for i in ids]

    # --- 純 Python バックエンド (参照実装) ------------------------------

    def _init_python(self, path):
        clip = _demo.ClipFile(path)
        self._clip = clip
        W, H, root = clip.canvas()
        self._root = root
        res = clip.cur.execute("SELECT CanvasResolution FROM Canvas").fetchone()[0]
        self.header = Header(W, H, float(res or 72.0))
        self._layer_columns = {d[1] for d in
                               clip.cur.execute("PRAGMA table_info(Layer)")}
        rows = {r[0]: r for r in clip.cur.execute(
            "SELECT MainId, LayerName, LayerFirstChildIndex, LayerNextIndex,"
            " LayerVisibility, LayerOpacity, LayerComposite, LayerFolder,"
            " LayerType, LayerClip FROM Layer")}
        self._rows = rows
        self.layers = []
        self._parent = {}
        self._children = {}

        def walk(parent_id):
            here = []
            child = rows[parent_id][2]
            while child:
                row = rows[child]
                inner = walk(child) if (row[7] & FOLDER_BIT) else []
                idx = len(self.layers)
                (mid, name, _fc, _nx, vis, opa, comp, folder, ltype, lclip) = row
                self.layers.append(Layer(
                    self, idx, mid, name, vis, opa, comp, lclip,
                    bool(folder & FOLDER_BIT), bool(ltype & FILTER_BIT),
                    self._is_text_py(mid), self._bounds_py(mid),
                    self._channels_py(mid)))
                for c in inner:
                    self._parent[c] = idx
                self._children[idx] = inner
                here.append(idx)
                child = row[3]
            return here

        for i in walk(root):
            self._parent[i] = -1
        self._children[-1] = [i for i, p in self._parent.items() if p == -1]

    def _is_text_py(self, main_id):
        if "TextLayerString" not in self._layer_columns:
            return False
        r = self._clip.cur.execute(
            "SELECT TextLayerString FROM Layer WHERE MainId=?", (main_id,)).fetchone()
        return bool(r and r[0])

    def _bounds_py(self, main_id):
        W, H = self.header.width, self.header.height
        if self._rows[main_id][7] & FOLDER_BIT:
            return 0, 0, 0, 0
        off = self._clip.top_offscreen(main_id)
        if off is None:
            return 0, 0, 0, 0
        if not self._clip.has_pixels(off) and \
                not self._clip.attribute(off)["has_init_color"]:
            obj = self._clip.object_offscreen(main_id)
            if obj is not None:
                oa = self._clip.attribute(obj)
                origin = self._clip.text_origin(main_id, oa["width"], oa["height"])
                if origin is not None:
                    return origin[0], origin[1], oa["width"], oa["height"]
        return 0, 0, W, H

    def _channels_py(self, main_id):
        if self._rows[main_id][7] & FOLDER_BIT:
            return []
        off = self._clip.top_offscreen(main_id)
        if off is None:
            return []
        a = self._clip.attribute(off)
        size = a["plane_bytes"] or (a["block_width"] * a["block_height"])
        n = a["num_channels"]
        ids = [-1, 0, 1, 2] if n == 4 else ([-1, 0] if n == 1 else [-1])
        return [Channel(i, size) for i in ids]

    # --- ツリービュー ---------------------------------------------------

    @property
    def roots(self):
        return self.children(-1)

    def children(self, index):
        return list(self._children.get(index, []))

    # --- 画像 -----------------------------------------------------------

    def merged_image(self):
        if self.backend == "cpp":
            return self._clip.merged_image()
        if self._composite is None:
            self._composite = _demo.composite(
                self._clip, self._root, self.header.width, self.header.height, 0)
        return self._composite[..., [2, 1, 0, 3]].tobytes()

    def layer_image(self, index, mode="masked"):
        if index < 0 or index >= len(self.layers):
            raise IndexError(index)
        if mode not in ("masked", "image", "mask"):
            raise ValueError(mode)
        lay = self.layers[index]
        if lay.width <= 0 or lay.height <= 0:
            return b""
        if self.backend == "cpp":
            return self._clip.layer_image(index, mode)
        key = (index, mode)
        if key not in self._image_cache:
            self._image_cache[key] = \
                self._layer_rgba_py(lay, mode)[..., [2, 1, 0, 3]].tobytes()
        return self._image_cache[key]

    def layer_region(self, index, x, y, width, height, mode="masked"):
        """レイヤの一部だけを読む。**CLIP は該当タイルしか展開しない。**

        PSD (psdparse) には無い口。同じことを PSD でやると `layer_image` を
        丸ごと読んでから切り出すことになる (行 RLE なので部分読みが効かない)。
        """
        if self.backend != "cpp":
            raise NotImplementedError("layer_region は C++ バックエンドのみ")
        return self._clip.layer_region(index, x, y, width, height, mode)

    def preview_image(self):
        """ファイルに埋まっている完成画。(png_bytes, w, h) か None。

        CLIP は `CanvasPreview`、PSD ならサムネイル image resource が相当する。
        """
        if self.backend == "cpp":
            return self._clip.preview_png()
        r = self._clip.cur.execute(
            "SELECT ImageData, ImageWidth, ImageHeight FROM CanvasPreview").fetchone()
        return (bytes(r[0]), r[1], r[2]) if r and r[0] else None

    def _layer_rgba_py(self, lay, mode):
        mid = lay.main_id
        W, H = self.header.width, self.header.height
        moff = self._clip.top_offscreen(mid, mask=True)
        has_mask = moff is not None and self._clip.has_pixels(moff)

        if mode == "mask":
            if not has_mask:
                return np.zeros((lay.height, lay.width, 4), np.uint8)
            m = _demo.fit_canvas(self._clip.offscreen_image(moff), W, H)
            out = np.zeros((H, W, 4), np.uint8)
            for c in range(3):
                out[..., c] = m[..., 3]
            out[..., 3] = 255
            return _crop(out, lay)

        img = _demo.layer_pixels(self._clip, mid, W, H, 0, "")
        if img is None:
            return np.zeros((lay.height, lay.width, 4), np.uint8)
        if img.shape[0] != lay.height or img.shape[1] != lay.width:
            img = _crop(_demo.fit_canvas(img, W, H), lay)
        if mode == "masked" and has_mask:
            m = _crop(_demo.fit_canvas(self._clip.offscreen_image(moff), W, H), lay)
            img = img.copy()
            img[..., 3] = (img[..., 3].astype(np.uint32)
                           * m[..., 3] // 255).astype(np.uint8)
        return img

    # 生のリーダ (CLIP 固有の機能へ抜ける口)
    @property
    def clip(self):
        return self._clip


def _crop(canvas, lay):
    out = np.zeros((lay.height, lay.width, 4), canvas.dtype)
    y0, x0 = max(0, lay.top), max(0, lay.left)
    y1 = min(canvas.shape[0], lay.top + lay.height)
    x1 = min(canvas.shape[1], lay.left + lay.width)
    if y1 > y0 and x1 > x0:
        out[y0 - lay.top:y1 - lay.top, x0 - lay.left:x1 - lay.left] = \
            canvas[y0:y1, x0:x1]
    return out


def open(path, backend=None):
    """拡張子で分岐して、psdparse 互換の読み取り面を返す。"""
    if str(path).lower().endswith(".clip"):
        return ClipDocument(str(path), backend)
    p = psdparse.PSDFile()
    if not p.load(str(path)):
        raise OSError(f"cannot load {path}")
    return p


def patch_psdparse():
    """`psdparse.PSDFile` を「拡張子で中身を切り替えるもの」に差し替える。

    psdparse の `examples/` や `tools/` は
    `p = psdparse.PSDFile(); p.load(path)` と書いてあるので、これを入れると
    **それらのスクリプトを 1 行も直さずに .clip へ向けられる**。
    共通面が足りているかの検証に使う。
    """
    real = psdparse.PSDFile

    class Dispatch:
        def __init__(self, *a, **kw):
            self._impl = None
            self._args = (a, kw)

        def load(self, path, *a, **kw):
            if str(path).lower().endswith(".clip"):
                self._impl = ClipDocument(str(path))
                return True
            a0, kw0 = self._args
            self._impl = real(*a0, **kw0)
            return self._impl.load(path, *a, **kw)

        def __getattr__(self, name):
            if self._impl is None:
                raise AttributeError(f"not loaded yet: {name}")
            return getattr(self._impl, name)

    psdparse.PSDFile = Dispatch
    return real
