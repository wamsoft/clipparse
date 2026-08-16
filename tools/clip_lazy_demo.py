"""遅延参照方式のプロトタイプ / 仕様の妥当性検証。

    python tools/clip_lazy_demo.py file.clip [-o out.png] [--compare ref.png]

**バイナリ領域を一切走査せず**、SQLite (ExternalChunk.Offset と
Offscreen.Attribute.BlockSize[]) から計算したオフセットだけで各ブロックへ
直接シークし、必要なブロックのみ zlib 展開する。docs/DESIGN.md §4.2 の経路を
そのまま Python で書いたもの。

読み出しの正しさは、取り出した各レイヤを下から順にストレートアルファ合成し、
CSP が書き出した参照 PNG と比較して確認する (--compare)。

依存: numpy, Pillow (検証用)。コア部分は標準ライブラリのみ。
"""

import argparse
import io
import sqlite3
import struct
import sys
import tempfile
import zlib

import numpy as np

VERBOSE = True                  # False にすると合成中のログを止める
BLOCK_BEGIN = "BlockDataBeginChunk".encode("utf-16be")
ALPHA_TILE = 64


def as_str(v):
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else str(v)


class ClipFile:
    """最小の遅延リーダ。raw は mmap の代用 (memoryview)。"""

    def __init__(self, path):
        self.raw = memoryview(open(path, "rb").read())

        # CHNKHead.binary_section_size が CHNKSQLi チャンクの位置そのもの。
        # 先頭 64 バイトだけで SQLite に直行できる (チャンク走査は不要)。
        magic, _filesize, hdrlen = struct.unpack_from(">8sQQ", self.raw, 0)
        if magic != b"CSFCHUNK":
            raise ValueError(f"not a .clip file (magic={magic!r})")
        _ctype, _clen = struct.unpack_from(">8sQ", self.raw, hdrlen)
        _ver, binary_section_size, _idlen = struct.unpack_from(">QQQ", self.raw, hdrlen + 16)

        ctype, dblen = struct.unpack_from(">8sQ", self.raw, binary_section_size)
        if ctype != b"CHNKSQLi":
            raise ValueError(f"binary_section_size does not point at CHNKSQLi ({ctype!r})")
        self.db_offset = binary_section_size + 16
        self.db_size = dblen

        # C++ 版は sqlite3_deserialize(..., SQLITE_DESERIALIZE_READONLY) で
        # mmap 上をゼロコピー参照する。Python の sqlite3 にその口がないので一時ファイル。
        f = tempfile.NamedTemporaryFile(delete=False, suffix=".sqlite")
        f.write(self.raw[self.db_offset: self.db_offset + self.db_size])
        f.close()
        self.tmp = f.name
        self.db = sqlite3.connect(f.name)
        self.cur = self.db.cursor()

        self.ext_offset = {as_str(e): o for e, o in
                           self.cur.execute("SELECT ExternalID, Offset FROM ExternalChunk")}
        self.attr_cache = {}

    # --- メタ情報 -------------------------------------------------------

    def canvas(self):
        """キャンバスの実ピクセルサイズとルートフォルダ ID。

        `Canvas.CanvasWidth/Height` は `CanvasUnit` の単位であってピクセルとは限らない
        (mm 指定のファイルが実在する)。実ピクセル寸法はルートフォルダの 100% ミップの
        Attribute から取るのが確実。
        """
        w, h, unit, res, root = self.cur.execute(
            "SELECT CanvasWidth, CanvasHeight, CanvasUnit, CanvasResolution, "
            "CanvasRootFolder FROM Canvas").fetchone()
        off = self.top_offscreen(root)
        if off is not None:
            a = self.attribute(off)
            return a["width"], a["height"], root
        return int(w), int(h), root         # ルートにミップが無い場合のみの退避

    def layer_order(self):
        """ルートフォルダの子チェーンを下から上の順で返す (フォルダは再帰)。"""
        rows = {r[0]: r for r in self.cur.execute(
            "SELECT MainId, LayerName, LayerFirstChildIndex, LayerNextIndex, "
            "LayerVisibility, LayerOpacity, LayerComposite, LayerFolder FROM Layer")}
        _w, _h, root = self.canvas()

        out = []

        def walk(mid, depth):
            child = rows[mid][2]
            while child:
                out.append((rows[child], depth))
                if rows[child][7] & 1:      # LayerFolder bit0 = フォルダ
                    walk(child, depth + 1)
                child = rows[child][3]

        walk(root, 0)
        return out

    def mipmap_chain(self, mipmap_id):
        """Mipmap.MainId から MipmapInfo 連鎖を辿って [(scale, offscreen_id)] を返す。

        `MipmapInfo WHERE LayerId=? AND ThisScale=100` で引いてはいけない。
        マスクを持つレイヤは描画用とマスク用の 2 本の連鎖を持ち、どちらも
        同じ LayerId・同じ ThisScale=100.0 で始まるため区別が付かない。
        """
        r = self.cur.execute(
            "SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?", (mipmap_id,)).fetchone()
        if not r:
            return []
        out, node = [], r[0]
        while node:
            scale, offs, nxt = self.cur.execute(
                "SELECT ThisScale, Offscreen, NextIndex FROM MipmapInfo WHERE MainId=?",
                (node,)).fetchone()
            out.append((scale, offs))
            node = nxt
        return out

    def top_offscreen(self, layer_id, mask=False):
        """描画用 (または マスク用) 100% ミップの Offscreen.MainId。"""
        col = "LayerLayerMaskMipmap" if mask else "LayerRenderMipmap"
        r = self.cur.execute(
            f"SELECT [{col}] FROM Layer WHERE MainId=?", (layer_id,)).fetchone()
        if not r or not r[0]:
            return None
        chain = self.mipmap_chain(r[0])
        return chain[0][1] if chain else None

    def thumbnail_offscreen(self, layer_id, mask=False):
        """MipmapInfo に載らないサムネイルキャッシュの Offscreen.MainId。"""
        col = "LayerLayerMaskThumbnail" if mask else "LayerRenderThumbnail"
        r = self.cur.execute(
            f"SELECT [{col}] FROM Layer WHERE MainId=?", (layer_id,)).fetchone()
        if not r or not r[0]:
            return None
        t = self.cur.execute(
            "SELECT ThumbnailOffscreen FROM LayerThumbnail WHERE MainId=?", (r[0],)).fetchone()
        return t[0] if t and t[0] else None

    def object_offscreen(self, layer_id):
        """ミップ段でもサムネイルでもない Offscreen (テキスト等の外接矩形ラスタ)。

        どのテーブルの FK からも参照されないので `Offscreen.LayerId` で引くしかない。
        """
        mip = {o for (o,) in self.cur.execute("SELECT Offscreen FROM MipmapInfo")}
        th = {o for (o,) in self.cur.execute(
            "SELECT ThumbnailOffscreen FROM LayerThumbnail") if o}
        for (m,) in self.cur.execute(
                "SELECT MainId FROM Offscreen WHERE LayerId=?", (layer_id,)):
            if m not in mip and m not in th and self.has_pixels(m):
                return m
        return None

    def text_origin(self, layer_id, w, h):
        """テキストレイヤの配置位置を `TextLayerAttributes` から取る。

        末尾の TLV ストリーム (u32 LE tag + u32 LE length + value) の
        タグ 42 が text_bbox = (x0, y0, x1, y1)。TLV の開始位置を求めるには
        手前のセクションを全部解く必要があるので、ここでは
        **矩形の大きさが対象 Offscreen と一致するもの**を探して同定する。
        """
        r = self.cur.execute(
            "SELECT TextLayerAttributes FROM Layer WHERE MainId=?", (layer_id,)).fetchone()
        if not r or not r[0]:
            return None
        b = bytes(r[0])
        for i in range(len(b) - 23):
            if struct.unpack_from("<I", b, i)[0] != 42:
                continue
            if struct.unpack_from("<I", b, i + 4)[0] != 16:
                continue
            x0, y0, x1, y1 = struct.unpack_from("<4i", b, i + 8)
            # 高さは text_bbox と Offscreen で食い違うことがある (行送り分?)。
            # 幅は両サンプルで一致したので、幅で同定して原点だけ採用する。
            if x1 - x0 == w - 1 and 0 <= x0 < 100000:
                return x0, y0
        return None

    def attribute(self, offscreen_id):
        if offscreen_id in self.attr_cache:
            return self.attr_cache[offscreen_id]
        attr, bd = self.cur.execute(
            "SELECT Attribute, BlockData FROM Offscreen WHERE MainId=?",
            (offscreen_id,)).fetchone()
        a = parse_attribute(bytes(attr))
        a["block_data_id"] = as_str(bd)
        # 前置和: block i のチャンク内相対オフセットを O(1) で引く
        acc, prefix = 0, []
        for s in a["block_sizes"]:
            prefix.append(acc)
            acc += s
        a["block_offsets"] = prefix
        self.attr_cache[offscreen_id] = a
        return a

    # --- 実データ (ここで初めてバイナリ領域を触る) ----------------------

    def has_pixels(self, offscreen_id):
        return self.attribute(offscreen_id)["block_data_id"] in self.ext_offset

    def read_block(self, offscreen_id, block_index):
        """ブロック 1 枚だけを展開する。他のブロックには一切触らない。"""
        a = self.attribute(offscreen_id)
        key = a["block_data_id"]
        if key not in self.ext_offset:
            return None

        chunk_off = self.ext_offset[key]
        data_off = chunk_off + 16 + 56          # CHNKExta ヘッダ 16 + 内部ヘッダ 56
        p = data_off + a["block_offsets"][block_index]

        size, namelen = struct.unpack_from(">II", self.raw, p)
        assert size == a["block_sizes"][block_index], "BlockSize table disagrees with stream"
        assert bytes(self.raw[p + 8: p + 8 + namelen * 2]) == BLOCK_BEGIN
        body = p + 8 + namelen * 2
        idx, declen, bw, bh, has = struct.unpack_from(">5I", self.raw, body)
        assert idx == block_index, (idx, block_index)
        if not has:
            return None

        section = struct.unpack_from(">I", self.raw, body + 20)[0]      # BE
        clen = struct.unpack_from("<I", self.raw, body + 24)[0]         # LE
        assert section == clen + 4
        assert 8 + namelen * 2 + 20 + 8 + clen + 4 + 34 == size, "record size formula"
        buf = zlib.decompress(self.raw[body + 28: body + 28 + clen])
        assert len(buf) == declen, (len(buf), declen)
        return decode_block(buf, a)

    def offscreen_image(self, offscreen_id):
        """1 offscreen の全面 RGBA。初期色があれば下地に敷く。"""
        a = self.attribute(offscreen_id)
        W, H = a["width"], a["height"]
        bw, bh, cols, rows = a["block_width"], a["block_height"], a["cols"], a["rows"]
        canvas = np.zeros((rows * bh, cols * bw, 4), np.uint8)
        if a["has_init_color"]:
            c = a["init_color"]
            canvas[...] = [(c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255]
        if a["block_data_id"] in self.ext_offset:
            for i in range(len(a["block_sizes"])):
                blk = self.read_block(offscreen_id, i)
                if blk is None:
                    continue
                r, c = divmod(i, cols)
                canvas[r * bh:(r + 1) * bh, c * bw:(c + 1) * bw] = blk
        return canvas[:H, :W]


def parse_attribute(a):
    out = {"section_sizes": struct.unpack_from(">IIII", a, 0)}
    p = 16
    for name in ("Parameter", "InitColor", "BlockSize"):
        p += 4                                              # boundary marker (=9)
        if a[p:p + 18].decode("utf-16be") != name:
            raise ValueError(f"missing {name} section")
        p += 18
        if name == "Parameter":
            v = struct.unpack_from(">20I", a, p)
            p += 80
            out.update(width=v[0], height=v[1], cols=v[2], rows=v[3],
                       color_mode=v[4], num_channels=v[6], bit_depth=v[7],
                       plane_bytes=v[8], plane_count=v[9], row_bytes=v[10],
                       block_height=v[14], block_width=v[15])
        elif name == "InitColor":
            p += 4                                          # magic (=20)
            q = struct.unpack_from(">IIII", a, p)
            p += 16
            out["has_init_color"], out["init_color"] = bool(q[0]), q[1]
            p += 4 * q[2]                                   # チャンネル別初期値 q[2] 個
        else:
            _magic, nblocks, _nchan = struct.unpack_from(">III", a, p)
            p += 12
            out["block_sizes"] = list(struct.unpack_from(f">{nblocks}I", a, p))
    return out


def decode_block(buf, a):
    """生ブロックを RGBA (bh, bw, 4) へ。表現色による 4 通りを吸収する。

    格納は「num_channels 枚のカラープレーン + アルファプレーン 1 枚」で、
    展開後サイズは `(plane_count + 1) * plane_bytes` になる
    (マスク面だけは幾何フィールドがゼロで、アルファ 1 枚のみ)。

    | color_mode | nch | bit | 構成                                        |
    |------------|-----|-----|---------------------------------------------|
    | 33         |  4  |  5  | rows[64:]=B,G,R,未使用 / rows[0:64]=折り畳みα |
    | 17         |  1  |  2  | plane0=α, plane1=グレー値 (8bpp)             |
    | 17         |  1  |  1  | plane0=α, plane1=値 (1bpp, 行 row_bytes)     |
    | 1          |  0  |  1  | 単一プレーン (マスク/選択, 8bpp)             |
    """
    bw, bh, nch = a["block_width"], a["block_height"], a["num_channels"]

    if nch == 4:
        return decode_rgba_block(buf, bw, bh)

    if nch == 0:
        # マスク / 選択範囲: アルファ 1 枚だけ。白 (=値 255) として返す。
        alpha = np.frombuffer(buf, np.uint8).reshape(bh, bw)
        out = np.full((bh, bw, 4), 255, np.uint8)
        out[..., 3] = alpha
        return out

    # グレー / モノクロ: プレーン 0 がアルファ、プレーン 1 が値
    if a["bit_depth"] == 1:
        planes = np.unpackbits(
            np.frombuffer(buf, np.uint8).reshape(2, a["plane_bytes"]), axis=1) * 255
        alpha = planes[0].reshape(bh, bw)
        value = planes[1].reshape(bh, bw)
    else:
        planes = np.frombuffer(buf, np.uint8).reshape(2, bh, bw)
        alpha, value = planes[0], planes[1]
    out = np.empty((bh, bw, 4), np.uint8)
    out[..., 0] = out[..., 1] = out[..., 2] = value
    out[..., 3] = alpha
    return out


def decode_rgba_block(buf, bw, bh):
    """(bh+64, bw, 4) の生ブロックを RGBA へ。

    rows[64:] が B,G,R,(未使用)、rows[0:64] が 4x4 スーパーピクセルに畳まれた
    アルファ面を幅 64 の 4 タイルに分割したもの。
    """
    img = np.frombuffer(buf, np.uint8).reshape(bh + ALPHA_TILE, bw, 4)
    color = img[ALPHA_TILE:].copy()
    tiles = [img[0:ALPHA_TILE, ALPHA_TILE * k: ALPHA_TILE * (k + 1)] for k in range(4)]
    alpha = (np.concatenate(tiles, axis=-1)
             .reshape(ALPHA_TILE, ALPHA_TILE, 4, 4)
             .swapaxes(1, 2).reshape(bh, bw))
    color[..., 3] = alpha
    color[:, :, [0, 2]] = color[:, :, [2, 0]]               # BGR -> RGB
    return color


def upscale(img, w, h):
    """サムネイルキャッシュを論理サイズへ最近傍で引き伸ばす (検証用の近似)。"""
    ys = (np.arange(h) * img.shape[0] // h).clip(0, img.shape[0] - 1)
    xs = (np.arange(w) * img.shape[1] // w).clip(0, img.shape[1] - 1)
    return img[ys][:, xs]


def fit_canvas(img, w, h):
    """キャンバスより大きい/小さい面を左上合わせで切り詰め・ゼロ埋めする。"""
    if img.shape[:2] == (h, w):
        return img
    out = np.zeros((h, w, img.shape[2]), img.dtype)
    ih, iw = min(h, img.shape[0]), min(w, img.shape[1])
    out[:ih, :iw] = img[:ih, :iw]
    return out


def hard_light(b, s):
    return np.where(s <= 0.5, 2 * b * s, 1 - 2 * (1 - b) * (1 - s))


def soft_light(b, s):
    d = np.where(b <= 0.25, ((16 * b - 12) * b + 4) * b, np.sqrt(np.maximum(b, 0)))
    return np.where(s <= 0.5, b - (1 - 2 * s) * b * (1 - b), b + (2 * s - 1) * (d - b))


def luma(c):
    return 0.3 * c[..., 0] + 0.59 * c[..., 1] + 0.11 * c[..., 2]


def set_luma(c, l):
    """c の輝度を l に置き換え、はみ出した成分を輝度中心に縮める。"""
    c = c + (l - luma(c))[..., None]
    lv = luma(c)[..., None]
    lo = c.min(axis=-1)[..., None]
    hi = c.max(axis=-1)[..., None]
    c = np.where(lo < 0, lv + (c - lv) * lv / np.maximum(lv - lo, 1e-9), c)
    c = np.where(hi > 1, lv + (c - lv) * (1 - lv) / np.maximum(hi - lv, 1e-9), c)
    return c


def set_sat(c, s):
    """彩度を s に置き換える (最小→0, 最大→s に線形写像)。"""
    mn = c.min(axis=-1)[..., None]
    mx = c.max(axis=-1)[..., None]
    return np.where(mx > mn, (c - mn) * s[..., None] / np.maximum(mx - mn, 1e-9), 0.0)


def sat_of(c):
    return c.max(axis=-1) - c.min(axis=-1)


def color_dodge(b, s):
    return np.where(s >= 1, 1.0, np.minimum(1.0, b / np.maximum(1 - s, 1e-9)))


def color_burn(b, s):
    return np.where(s <= 0, 0.0, 1 - np.minimum(1.0, (1 - b) / np.maximum(s, 1e-9)))


def pick_by_luma(b, s, take_max):
    lb, ls = luma(b)[..., None], luma(s)[..., None]
    return np.where((ls > lb) if take_max else (ls < lb), s, b)


# `Layer.LayerComposite` → 合成式。samples/blendmodes.clip と samples/blend2.clip の
# 各モードを CanvasPreview と突き合わせて実測同定した (CLIP_FORMAT.md §9)。
# 未測定のモードはここに無い = 通常合成にフォールバックする。
BLEND = {
    1:  lambda b, s: np.minimum(b, s),                        # 比較 (暗)
    2:  lambda b, s: b * s,                                   # 乗算
    3:  color_burn,                                           # 焼きこみカラー
    4:  lambda b, s: b + s - 1,                               # 焼きこみ (リニア)
    5:  lambda b, s: b - s,                                   # 減算
    6:  lambda b, s: pick_by_luma(b, s, False),               # カラー比較 (暗)
    7:  lambda b, s: np.maximum(b, s),                        # 比較 (明)
    8:  lambda b, s: b + s - b * s,                           # スクリーン
    9:  color_dodge,                                          # 覆い焼きカラー
    10: color_dodge,                                          # 覆い焼き (発光)
    11: lambda b, s: b + s,                                   # 加算
    12: lambda b, s: b + s,                                   # 加算 (発光)
    13: lambda b, s: pick_by_luma(b, s, True),                # カラー比較 (明)
    14: lambda b, s: hard_light(s, b),                        # オーバーレイ
    15: soft_light,                                           # ソフトライト
    16: hard_light,                                           # ハードライト
    17: lambda b, s: np.where(s <= 0.5, color_burn(b, 2 * s),  # ビビッドライト
                              color_dodge(b, 2 * (s - 0.5))),
    18: lambda b, s: b + 2 * s - 1,                           # リニアライト
    19: lambda b, s: np.where(s <= 0.5, np.minimum(b, 2 * s),  # ピンライト
                              np.maximum(b, 2 * s - 1)),
    20: lambda b, s: (b + s >= 1.0).astype(np.float64),       # ハードミックス
    21: lambda b, s: np.abs(b - s),                           # 差の絶対値
    22: lambda b, s: b + s - 2 * b * s,                       # 除外
    23: lambda b, s: set_luma(set_sat(s, sat_of(b)), luma(b)),  # 色相
    24: lambda b, s: set_luma(set_sat(b, sat_of(s)), luma(b)),  # 彩度
    25: lambda b, s: set_luma(s, luma(b)),                    # カラー
    26: lambda b, s: set_luma(b, luma(s)),                    # 輝度
    36: lambda b, s: b / np.maximum(s, 1e-9),                 # 除算
}


def blend_over(dst, src, mode):
    """一般形の合成 (下地が透明でも正しい)。dst/src とも RGBA uint8 ストレートα。

        αo = αs + αb*(1-αs)
        Co*αo = (1-αb)*αs*Cs + αb*αs*B(Cb,Cs) + (1-αs)*αb*Cb

    下地が不透明 (αb=1) なら `(1-αs)*Cb + αs*B` に帰着する。
    透明な下地 (αb=0) では合成式によらず通常合成になる —
    分離フォルダ (通過でないフォルダ) の最下段レイヤがこれに当たる。
    """
    ab = dst[..., 3:4].astype(np.float64) / 255.0
    a_s = src[..., 3:4].astype(np.float64) / 255.0
    b = dst[..., :3].astype(np.float64) / 255.0
    s = src[..., :3].astype(np.float64) / 255.0

    ao = a_s + ab * (1 - a_s)

    if mode in GLOW_MODES:
        # 「発光」モード: **s を α で乗じてから、α による補間をせずに**直接ブレンドする。
        # [実測: samples/glow.clip] 覆い焼き(発光) は覆い焼きカラーと式は同じ
        # `min(1, b/(1-s))` だが、α の入れ方がこちらだと max=1 で一致する
        # (通常の補間だと max=3 ずれる)。
        # 加算系は線形なので両者が代数的に一致し、11 と 12 は区別が付かない。
        glow = np.clip(BLEND[mode](b, s * a_s), 0.0, 1.0)
        num = (1 - ab) * a_s * s + ab * glow
        out = np.where(ao > 0, num / np.maximum(ao, 1e-9), 0.0)
        dst[..., :3] = np.clip(np.round(out * 255.0), 0, 255).astype(np.uint8)
        dst[..., 3] = np.clip(np.round(ao[..., 0] * 255.0), 0, 255).astype(np.uint8)
        return dst

    blended = np.clip(BLEND[mode](b, s), 0.0, 1.0) if mode in BLEND else s
    num = (1 - ab) * a_s * s + ab * a_s * blended + (1 - a_s) * ab * b
    out = np.where(ao > 0, num / np.maximum(ao, 1e-9), 0.0)

    dst[..., :3] = np.clip(np.round(out * 255.0), 0, 255).astype(np.uint8)
    dst[..., 3] = np.clip(np.round(ao[..., 0] * 255.0), 0, 255).astype(np.uint8)
    return dst


def place(img, origin, w, h):
    """外接矩形サイズのラスタをキャンバス上の (x, y) に置く。はみ出しは切り詰める。"""
    x, y = origin
    out = np.zeros((h, w, img.shape[2]), img.dtype)
    sx, sy = max(0, -x), max(0, -y)
    dx, dy = max(0, x), max(0, y)
    cw = min(img.shape[1] - sx, w - dx)
    ch = min(img.shape[0] - sy, h - dy)
    if cw > 0 and ch > 0:
        out[dy:dy + ch, dx:dx + cw] = img[sy:sy + ch, sx:sx + cw]
    return out


def src_over(dst, src):
    """ストレートアルファの通常合成。"""
    a = src[..., 3:4].astype(np.float32) / 255.0
    dst[..., :3] = (src[..., :3] * a + dst[..., :3] * (1 - a)).round().astype(np.uint8)
    dst[..., 3] = np.maximum(dst[..., 3], src[..., 3])
    return dst


def layer_pixels(clip, mid, W, H, depth, tag):
    """1 レイヤの RGBA をキャンバスサイズで返す。取れなければ None。

    100% ミップに実体が無いときの退避先:
      1) テキスト等のオブジェクトレイヤ … 外接矩形ラスタを配置位置に置く
      2) グループ                       … サムネイルキャッシュを拡大
    """
    off = clip.top_offscreen(mid)
    if off is None:
        return None
    fallback = obj = origin = None
    if not clip.has_pixels(off) and not clip.attribute(off)["has_init_color"]:
        obj = clip.object_offscreen(mid)
        if obj is not None:
            oa = clip.attribute(obj)
            origin = clip.text_origin(mid, oa["width"], oa["height"])
            if origin is None:
                obj = None
        if obj is None:
            thumb = clip.thumbnail_offscreen(mid)
            if thumb is not None and clip.has_pixels(thumb):
                fallback = thumb
    a = clip.attribute(off)
    note = (f" -> thumbnail #{fallback}" if fallback else
            f" -> object #{obj}@{origin}" if obj else "")
    if VERBOSE:
        print(f"  {'  ' * depth}{tag} #{mid}: offscreen=#{off} {a['width']}x{a['height']} "
          f"blocks={len(a['block_sizes'])} init={'yes' if a['has_init_color'] else 'no'} "
              f"pixels={'yes' if clip.has_pixels(off) else 'NO'}{note} "
              f"mode=(cm{a['color_mode']},nch{a['num_channels']},bd{a['bit_depth']})")

    if obj is not None:
        return fit_canvas(place(clip.offscreen_image(obj), origin, W, H), W, H).copy()
    if fallback is not None:
        return fit_canvas(upscale(clip.offscreen_image(fallback),
                                  a["width"], a["height"]), W, H).copy()
    if not clip.has_pixels(off) and not a["has_init_color"]:
        return None
    return fit_canvas(clip.offscreen_image(off), W, H).copy()


PASS_THROUGH = 30               # LayerComposite。フォルダ専用
GLOW_MODES = {10, 12}           # 「発光」付き: 覆い焼き(発光) / 加算(発光)
FILTER_BIT = 4096               # LayerType。調整レイヤ


def parse_filter_info(blob):
    """`Layer.FilterLayerInfo` を解く。

        u32 BE  filter_type      1=明るさ・コントラスト 2=レベル補正 3=トーンカーブ
                                 4=色相・彩度・明度 5=カラーバランス 6=階調の反転
                                 7=ポスタリゼーション 8=2値化 9=グラデーションマップ
        u32 BE  payload_bytes
        i32 BE  params[payload_bytes / 4]

    実測値: 明るさ+50 → (1, 8, [50, 0]) / 明るさ+100 → (1, 8, [100, 0]) /
    2値化128 → (8, 4, [128]) / 色相+30 → (4, 12, [30, 0, 0])
    """
    ftype, nbytes = struct.unpack_from(">II", blob, 0)
    n = nbytes // 4
    return ftype, list(struct.unpack_from(f">{n}i", blob, 8)) if n else []


def rgb_to_hsv(c):
    mx = c.max(-1); mn = c.min(-1); d = mx - mn
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    h = np.zeros_like(mx)
    h = np.where((mx == r) & (d > 0), ((g - b) / np.maximum(d, 1e-9)) % 6, h)
    h = np.where((mx == g) & (d > 0), (b - r) / np.maximum(d, 1e-9) + 2, h)
    h = np.where((mx == b) & (d > 0), (r - g) / np.maximum(d, 1e-9) + 4, h)
    return h * 60, np.where(mx > 0, d / np.maximum(mx, 1e-9), 0.0), mx


def hsv_to_rgb(h, s, v):
    h = (h % 360) / 60
    i = np.floor(h).astype(int); f = h - i
    p = v * (1 - s); q = v * (1 - s * f); t = v * (1 - s * (1 - f))
    out = np.zeros(h.shape + (3,))
    for k, tri in enumerate([(v, t, p), (q, v, p), (p, v, t),
                             (p, q, v), (t, p, v), (v, p, q)]):
        m = (i % 6) == k
        for ch in range(3):
            out[..., ch] = np.where(m, tri[ch], out[..., ch])
    return out


TONE_BLOCK_U16 = 65             # 1 + 32 点 x (x, y)。ブロックは 32 本並ぶ


def tone_curve_lut(blob):
    """トーンカーブ (種別 3) の合成チャンネル LUT を作る。

    payload は `u16 BE count + 32 x (u16 BE x, u16 BE y)` のブロックが
    32 本並んだもの。先頭ブロックが合成チャンネル、以降が R/G/B。

    **格納された点は曲線が通る点ではなく、ベジェの制御点** [実測]。
    3 点 (= 2 次ベジェ) で `filter.clip` が max=1 で一致した。
    2 点なら直線なので線形補間と同じ。
    """
    nbytes = struct.unpack_from(">I", blob, 4)[0]
    u16 = struct.unpack_from(f">{nbytes // 2}H", blob, 8)
    count = u16[0]
    if count < 2:
        return None
    pts = np.array([[u16[1 + 2 * i], u16[2 + 2 * i]] for i in range(count)],
                   dtype=np.float64) / 65535.0 * 255.0

    # Bernstein 基底で次数 count-1 のベジェを描き、x で並べ替えて LUT にする
    t = np.linspace(0.0, 1.0, 20001)
    n = count - 1
    from math import comb
    bx = np.zeros_like(t); by = np.zeros_like(t)
    for i in range(count):
        w = comb(n, i) * (1 - t) ** (n - i) * t ** i
        bx += w * pts[i, 0]
        by += w * pts[i, 1]
    return np.interp(np.arange(256), bx, by)


def apply_filter(dst, blob):
    """調整レイヤを下地 (dst) に適用する。dst は RGBA uint8。

    検証済み: 1 明るさ (加算) / 3 トーンカーブ (ベジェ) /
    4 色相 (HSV 度数回転) / 6 階調の反転 / 8 2値化 (チャンネルごとの閾値)。
    4 の彩度・明度は**未確定** (§CLIP_FORMAT.md 10)。
    """
    ftype, p = parse_filter_info(blob)
    rgb = dst[..., :3].astype(np.float64)
    if ftype == 1 and len(p) >= 1:                  # 明るさ・コントラスト
        out = rgb + p[0]                            # コントラスト p[1] は未検証
    elif ftype == 3:                                # トーンカーブ
        lut = tone_curve_lut(blob)
        if lut is None:
            return dst
        out = lut[np.clip(rgb, 0, 255).astype(int)]
    elif ftype == 4 and len(p) >= 3:                # 色相・彩度・明度
        h, s, v = rgb_to_hsv(rgb / 255.0)
        if p[1]:                                    # 彩度: 最良近似 (max=11 残る)
            s = np.clip(s + (1 - s) * (p[1] / 100.0), 0, 1) if p[1] > 0 else \
                np.clip(s * (1 + p[1] / 100.0), 0, 1)
        out = hsv_to_rgb(h + p[0], s, v) * 255.0    # 明度 p[2] は未実装
    elif ftype == 6:                                # 階調の反転
        out = 255.0 - rgb
    elif ftype == 8 and len(p) >= 1:                # 2値化
        out = np.where(rgb >= p[0], 255.0, 0.0)
    else:
        return dst                                  # 未対応の種別は素通し
    dst[..., :3] = np.clip(np.round(out), 0, 255).astype(np.uint8)
    return dst


def composite(clip, parent_id, W, H, depth, dst=None):
    """parent_id の子を下から順に合成した RGBA を返す。

    フォルダの扱いが 2 通りある:
      - **通過** (`LayerComposite == 30`) … 子を**呼び出し元の**バッファへ直接描く。
        分離しないので、中のレイヤの合成モードがフォルダの外の下地にも効く
      - それ以外 … 透明なバッファに子を描いてから、フォルダ自身の
        合成モード / 不透明度 / マスクで親へ重ねる。中の合成はフォルダ内で閉じる
    """
    if dst is None:
        dst = np.zeros((H, W, 4), np.uint8)
    clip_base = None            # クリッピングの土台となる直近の非クリップレイヤのα

    rows = {r[0]: r for r in clip.cur.execute(
        "SELECT MainId, LayerName, LayerFirstChildIndex, LayerNextIndex, LayerVisibility, "
        "LayerOpacity, LayerComposite, LayerFolder, LayerType, LayerClip FROM Layer")}

    child = rows[parent_id][2]
    while child:
        (mid, name, _fc, nxt, vis, opa, comp, folder, ltype, lclip) = rows[child]
        child = nxt
        is_folder = bool(folder & 1)
        tag = ("folder" if is_folder else "layer ") + f" {name!r}"

        if not vis:
            if VERBOSE:
                print(f"  {'  ' * depth}{tag} #{mid}: (非表示)")
            continue

        if is_folder and comp == PASS_THROUGH:
            # 通過フォルダ: 分離せず、この階層のバッファへ直接描き込む
            if VERBOSE:
                print(f"  {'  ' * depth}{tag} #{mid}: 通過フォルダ")
            composite(clip, mid, W, H, depth + 1, dst)
            clip_base = None
            continue

        if ltype & FILTER_BIT:
            # 調整レイヤ: 画素を持たず、下にある結果を書き換える
            blob = clip.cur.execute(
                "SELECT FilterLayerInfo FROM Layer WHERE MainId=?", (mid,)).fetchone()
            if blob and blob[0]:
                ftype, params = parse_filter_info(bytes(blob[0]))
                if VERBOSE:
                    print(f"  {'  ' * depth}{tag} #{mid}: 調整レイヤ type={ftype} params={params}")
                dst = apply_filter(dst, bytes(blob[0]))
            continue

        if is_folder:
            img = composite(clip, mid, W, H, depth + 1)
        else:
            img = layer_pixels(clip, mid, W, H, depth, tag)
            if img is None:
                continue

        # レイヤマスク: LayerType bit1 が立っていればマスク連鎖を掛ける
        if ltype & 2:
            moff = clip.top_offscreen(mid, mask=True)
            if moff is not None and clip.has_pixels(moff):
                mimg = fit_canvas(clip.offscreen_image(moff), W, H)
                img[..., 3] = (img[..., 3].astype(np.uint32)
                               * mimg[..., 3] // 255).astype(np.uint8)

        if opa < 256:                       # LayerOpacity は 0..256 (255 ではない)
            img[..., 3] = (img[..., 3].astype(np.uint32) * opa // 256).astype(np.uint8)

        # クリッピング: 直下の非クリップレイヤのαで切り抜く
        if lclip:
            if clip_base is not None:
                img[..., 3] = ((img[..., 3].astype(np.uint32)
                                * clip_base + 127) // 255).astype(np.uint8)
        else:
            clip_base = img[..., 3].copy()

        dst = blend_over(dst, img, comp)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("-o", "--out", help="合成結果の PNG 出力先")
    ap.add_argument("--compare", help="比較する参照 PNG")
    ap.add_argument("--preview", action="store_true",
                    help="ファイル内蔵の CanvasPreview を正解として比較する")
    args = ap.parse_args()

    clip = ClipFile(args.path)
    W, H, root = clip.canvas()
    print(f"canvas {W}x{H}  root layer #{root}")
    print(f"sqlite region: offset={clip.db_offset} size={clip.db_size} "
          f"({100 * clip.db_size / len(clip.raw):.1f}% of file)")
    print(f"external chunks with data: {len(clip.ext_offset)}")

    out = composite(clip, root, W, H, 0)

    if args.out or args.compare or args.preview:
        from PIL import Image
        if args.out:
            Image.fromarray(out).save(args.out)
            print(f"\nwrote {args.out}")

        ref = label = None
        if args.compare:
            ref = Image.open(args.compare).convert("RGB")
            label = args.compare
        elif args.preview:
            blob = clip.cur.execute(
                "SELECT ImageData FROM CanvasPreview").fetchone()[0]
            ref = Image.open(io.BytesIO(blob)).convert("RGB")
            label = f"CanvasPreview ({ref.size[0]}x{ref.size[1]})"

        if ref is not None:
            mine = Image.fromarray(out[..., :3])
            if mine.size != ref.size:            # プレビューは縮小されていることがある
                mine = mine.resize(ref.size, Image.LANCZOS)
                label += " [縮小して比較]"
            d = np.abs(np.array(mine).astype(int) - np.array(ref).astype(int))
            print(f"\nvs {label}: mean={d.mean():.4f} max={d.max()} "
                  f"pixels differing by >2: {(d.max(axis=2) > 2).sum()}")
            return 0 if d.max() <= 2 else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
