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

依存: numpy / psdparse (enum を借りる) / clip_lazy_demo。
"""

import importlib.util
import os

import numpy as np
import psdparse

_spec = importlib.util.spec_from_file_location(
    "clip_lazy_demo", os.path.join(os.path.dirname(__file__), "clip_lazy_demo.py"))
demo = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(demo)
demo.VERBOSE = False


# CLIP の LayerComposite → PSD の 4 文字キー (docs/CLIP_FORMAT.md §9 で実測同定)
BLEND_TO_PSD = {
    0: "norm", 1: "dark", 2: "mul ", 3: "idiv", 4: "lbrn", 5: "fsub", 6: "dkCl",
    7: "lite", 8: "scrn", 9: "div ", 10: "div ", 11: "lddg", 12: "lddg",
    13: "lgCl", 14: "over", 15: "sLit", 16: "hLit", 17: "vLit", 18: "lLit",
    19: "pLit", 20: "hMix", 21: "diff", 22: "smud", 23: "hue ", 24: "sat ",
    25: "colr", 26: "lum ", 30: "pass", 36: "fdiv",
}

FILTER_BIT = 4096
FOLDER_BIT = 1
PAPER_TYPE = 1584


def _key_to_int(key):
    return sum(ord(c) << s for c, s in zip(key, (24, 16, 8, 0)))


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

    def __init__(self, doc, index, row):
        self._doc = doc
        self._index = index
        (self.main_id, name, _fc, _nx, vis, opa, comp, folder,
         ltype, lclip) = row
        self.name_unicode = name or ""
        self.name = self.name_unicode
        self.layer_id = self.main_id
        self.visible = bool(vis)
        # CLIP は 0..256、PSD は 0..255
        self.opacity = min(255, opa * 255 // 256)
        self.opacity_raw = opa                      # CLIP 固有 (0..256)
        self.clipping = 1 if lclip else 0
        self.composite_raw = comp                   # CLIP 固有 (LayerComposite)
        self.blend_mode_key = _key_to_int(BLEND_TO_PSD.get(comp, "norm"))
        self.blend_mode = _blend_mode_of(comp)
        self.is_group = bool(folder & FOLDER_BIT)
        self.transparency_protected = False
        self.obsolete = False
        self.pixel_data_irrelevant = False
        self.fill_opacity = 255

        if self.is_group:
            self.layer_type = psdparse.LayerType.FOLDER
        elif ltype & FILTER_BIT:
            self.layer_type = psdparse.LayerType.ADJUST
        elif doc._is_text(self.main_id):
            self.layer_type = psdparse.LayerType.TEXT
        else:
            self.layer_type = psdparse.LayerType.NORMAL

        self.left, self.top, self.width, self.height = doc._bounds(self.main_id)
        self.right = self.left + self.width
        self.bottom = self.top + self.height
        self.channels = doc._channels(self.main_id)

    @property
    def parent_index(self):
        return self._doc._parent.get(self._index, -1)

    @property
    def children(self):
        return self._doc.children(self._index)

    def __repr__(self):
        return (f"<Layer {self._index} {self.name_unicode!r} "
                f"{self.width}x{self.height} {self.layer_type}>")


def _blend_mode_of(comp):
    key = BLEND_TO_PSD.get(comp, "norm")
    # psdparse の BlendMode enum は 4cc から引けないので名前で対応させる
    names = {
        "norm": "NORMAL", "dark": "DARKEN", "mul ": "MULTIPLY",
        "idiv": "COLOR_BURN", "lbrn": "LINEAR_BURN", "fsub": "SUBTRACT",
        "dkCl": "DARKER_COLOR", "lite": "LIGHTEN", "scrn": "SCREEN",
        "div ": "COLOR_DODGE", "lddg": "LINEAR_DODGE", "lgCl": "LIGHTER_COLOR",
        "over": "OVERLAY", "sLit": "SOFT_LIGHT", "hLit": "HARD_LIGHT",
        "vLit": "VIVID_LIGHT", "lLit": "LINEAR_LIGHT", "pLit": "PIN_LIGHT",
        "hMix": "HARD_MIX", "diff": "DIFFERENCE", "smud": "EXCLUSION",
        "hue ": "HUE", "sat ": "SATURATION", "colr": "COLOR", "lum ": "LUMINOSITY",
        "pass": "PASS_THROUGH", "fdiv": "DIVIDE",
    }
    return getattr(psdparse.BlendMode, names.get(key, "NORMAL"))


class ClipDocument:
    """psdparse.PSDFile 互換の読み取り面を .clip に被せたもの。"""

    def __init__(self, path):
        self._clip = demo.ClipFile(path)
        W, H, root = self._clip.canvas()
        self._root = root
        res = self._clip.cur.execute(
            "SELECT CanvasResolution FROM Canvas").fetchone()[0]
        self.header = Header(W, H, float(res or 72.0))
        self.merged_alpha = True
        self.is_loaded = True
        self._composite = None
        self._image_cache = {}

        rows = {r[0]: r for r in self._clip.cur.execute(
            "SELECT MainId, LayerName, LayerFirstChildIndex, LayerNextIndex,"
            " LayerVisibility, LayerOpacity, LayerComposite, LayerFolder,"
            " LayerType, LayerClip FROM Layer")}
        self._rows = rows

        self._layer_columns = {d[1] for d in
                               self._clip.cur.execute("PRAGMA table_info(Layer)")}

        # CLIP のツリーを psdparse と同じ「平坦・下から上」の並びへ均す。
        # 中身を先に、フォルダ自身を後に積むので PSD の並び順と一致する
        # (PSD はフォルダレイヤが中身より上にある)。
        self.layers = []
        self._parent = {}
        self._children = {}

        def walk(parent_id):
            """parent_id の子を積み、この階層で積んだ index を返す。"""
            here = []
            child = rows[parent_id][2]
            while child:
                row = rows[child]
                inner = walk(child) if (row[7] & FOLDER_BIT) else []
                idx = len(self.layers)
                self.layers.append(Layer(self, idx, row))
                for c in inner:
                    self._parent[c] = idx
                self._children[idx] = inner
                here.append(idx)
                child = row[3]
            return here

        for i in walk(root):
            self._parent[i] = -1
        self._children[-1] = [i for i, p in self._parent.items() if p == -1]

    # --- ツリービュー ---------------------------------------------------

    @property
    def roots(self):
        return self.children(-1)

    def children(self, index):
        return list(self._children.get(index, []))

    # --- 画像 -----------------------------------------------------------

    def merged_image(self):
        if self._composite is None:
            self._composite = demo.composite(
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
        key = (index, mode)
        if key not in self._image_cache:
            rgba = self._layer_rgba(lay, mode)
            self._image_cache[key] = rgba[..., [2, 1, 0, 3]].tobytes()
        return self._image_cache[key]

    def _layer_rgba(self, lay, mode):
        mid = lay.main_id
        W, H = self.header.width, self.header.height
        moff = self._clip.top_offscreen(mid, mask=True)
        has_mask = moff is not None and self._clip.has_pixels(moff)

        if mode == "mask":
            if not has_mask:
                return np.zeros((lay.height, lay.width, 4), np.uint8)
            m = demo.fit_canvas(self._clip.offscreen_image(moff), W, H)
            out = np.zeros((H, W, 4), np.uint8)
            for c in range(3):
                out[..., c] = m[..., 3]
            out[..., 3] = 255
            return _crop(out, lay)

        img = demo.layer_pixels(self._clip, mid, W, H, 0, "")
        if img is None:
            return np.zeros((lay.height, lay.width, 4), np.uint8)
        if img.shape[0] != lay.height or img.shape[1] != lay.width:
            img = _crop(demo.fit_canvas(img, W, H), lay)
        if mode == "masked" and has_mask:
            m = _crop(demo.fit_canvas(self._clip.offscreen_image(moff), W, H), lay)
            img = img.copy()
            img[..., 3] = (img[..., 3].astype(np.uint32)
                           * m[..., 3] // 255).astype(np.uint8)
        return img

    # --- 内部 -----------------------------------------------------------

    def _is_text(self, main_id):
        # Layer の列構成はファイル (CSP のバージョンと使用機能) で変わるので、
        # 列の有無を確かめてから引く。
        if "TextLayerString" not in self._layer_columns:
            return False
        r = self._clip.cur.execute(
            "SELECT TextLayerString FROM Layer WHERE MainId=?", (main_id,)).fetchone()
        return bool(r and r[0])

    def _bounds(self, main_id):
        """レイヤのラスタがキャンバス上で占める矩形。

        CLIP のラスタは基本キャンバス全面だが、テキスト等のオブジェクトレイヤは
        外接矩形サイズの Offscreen を配置位置に置く (CLIP_FORMAT.md §2.3)。
        """
        W, H = self.header.width, self.header.height
        if self._rows[main_id][7] & FOLDER_BIT:
            return 0, 0, 0, 0        # フォルダは画素を持たない (psdparse と同じ)
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

    def _channels(self, main_id):
        """CLIP のプレーン構成を PSD のチャンネル id で見せる。

        CLIP はチャンネル分解ではなくブロック内のプレーンで持っている
        (CLIP_FORMAT.md §3.2.1)。id は PSD の慣習 (-1=α, 0/1/2=R/G/B) に合わせ、
        length は 1 プレーンの展開後バイト数を入れる。
        """
        if self._rows[main_id][7] & FOLDER_BIT:
            return []
        off = self._clip.top_offscreen(main_id)
        if off is None:
            return []
        a = self._clip.attribute(off)
        n = a["num_channels"]
        size = a["plane_bytes"] or (a["block_width"] * a["block_height"])
        if n == 4:
            ids = [-1, 0, 1, 2]
        elif n == 1:
            ids = [-1, 0]                       # グレー/モノクロ: α + 値
        else:
            ids = [-1]                          # マスク: α のみ
        return [Channel(i, size) for i in ids]

    # 生の clipparse リーダ (CLIP 固有の機能へ抜ける口)
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


def open(path):
    """拡張子で分岐して、psdparse 互換の読み取り面を返す。"""
    if str(path).lower().endswith(".clip"):
        return ClipDocument(str(path))
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
