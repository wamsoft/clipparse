"""空の `.clip` を雛形にして**任意サイズのキャンバスを組み立てる** (W4 の土台)。

`clip_write.py` の編集系はどれも「既にある構造の一部を差し替える」ものだった。
ここは一段上で、**キャンバスの寸法そのものを作り替える**。

    from clip_build import mip_levels, retarget_attribute, resize_canvas

**なぜ雛形方式か**: `Layer` 57 列 / `Offscreen` 6 列 / `Canvas` 35 列のうち、
CSP が期待する既定値の大半は意味が分かっていない。ゼロから行を書くより
**空ファイルの行を作り替える**方が安全で、分かっている列だけ触れば済む。
雛形は `samples/emptyimage.clip` (CSP で「新規」しただけのファイル)。

**ミップ段数の決まり方** [実測: 5 ファイルで一致]。100% から `//2` で縮小し、
**ブロックグリッドが 1x1 になった段の次の段まで**作る:

    1400x700 -> 700x350 -> 350x175 -> 175x87 -> 87x43        (6x3,3x2,2x1,1x1,1x1)
    800x1000 -> 400x500 -> 200x250 -> 100x125                (4x4,2x2,1x1,1x1)
    300x400  -> 150x200 -> 75x100                            (2x2,1x1,1x1)

サムネイルは**キャンバス寸法によらず 512x512 固定** [実測]。

依存: 標準ライブラリのみ。
"""

import struct

BLOCK_W = BLOCK_H = 256
EMPTY_RECORD_SIZE = 104


def mip_levels(width, height):
    """100% から順に (w, h) を返す。上記の実測則。"""
    levels = []
    w, h = width, height
    while True:
        levels.append((w, h))
        if len(levels) >= 2 and _grid(*levels[-1]) == (1, 1) and \
                _grid(*levels[-2]) == (1, 1):
            return levels
        w, h = max(1, w // 2), max(1, h // 2)


def _grid(w, h):
    return (-(-w // BLOCK_W), -(-h // BLOCK_H))


def split_attribute(attr):
    """Attribute BLOB を (section_sizes, [各セクションの生バイト]) に割る。"""
    sizes = list(struct.unpack_from(">4I", attr, 0))
    out, p = [], sizes[0]
    for n in sizes[1:]:
        out.append(attr[p:p + n])
        p += n
    if p != len(attr):
        raise ValueError(f"Attribute の長さが合わない: {p} != {len(attr)}")
    return sizes, out


def retarget_attribute(attr, width, height, block_sizes=None):
    """既存の Attribute を別の寸法に作り替える。

    `Parameter` の幅・高さ・グリッドだけ書き換え、`BlockSize` を作り直す。
    表現色・初期色などの分かっていない定数は**そのまま持ち越す**。
    `block_sizes` 省略時は全ブロック空 (104)。
    """
    sizes, sec = split_attribute(attr)
    cols, rows = _grid(width, height)
    nblocks = cols * rows
    if block_sizes is None:
        block_sizes = [EMPTY_RECORD_SIZE] * nblocks
    elif len(block_sizes) != nblocks:
        raise ValueError(f"ブロック数が合わない: {len(block_sizes)} != {nblocks}")

    param = bytearray(sec[0])
    struct.pack_into(">4I", param, 22, width, height, cols, rows)  # 4 + 18

    nchan = struct.unpack_from(">I", sec[2], 22 + 8)[0]            # 既存値を保つ
    blk = (sec[2][:22] + struct.pack(">3I", 12, nblocks, nchan)
           + struct.pack(f">{nblocks}I", *block_sizes))

    new = [bytes(param), sec[1], blk]
    return (struct.pack(">4I", 16, *(len(x) for x in new)) + b"".join(new))


def attribute_dims(attr):
    """(width, height, cols, rows) を返す。"""
    _sizes, sec = split_attribute(attr)
    return struct.unpack_from(">4I", sec[0], 22)


def resize_canvas(db, width, height, dpi=None):
    """キャンバスを `width x height` に作り替える。

    - `Canvas` の寸法を更新する
    - 全レイヤのミップ連鎖を新しい段数に合わせて**伸縮**する
      (足りなければ `MipmapInfo` + `Offscreen` を複製して足し、余れば消す)
    - 全 `Offscreen` の `Attribute` を新しい寸法へ作り替え、**中身は空にする**
      (実体は呼び出し側が入れ直す。CSP は開いた時に再生成もしてくれる)

    戻り値は新しいミップ段のリスト。**`Offscreen.BlockData` の実体
    (`CHNKExta`) は消えるので、呼び出し側で `externals` を作り直すこと。**
    """
    cur = db.cursor()
    canvas_id = cur.execute("SELECT MainId FROM Canvas").fetchone()[0]
    levels = mip_levels(width, height)

    cur.execute("UPDATE Canvas SET CanvasWidth=?, CanvasHeight=?, CanvasUnit=0"
                " WHERE MainId=?", (float(width), float(height), canvas_id))
    if dpi is not None:
        cur.execute("UPDATE Canvas SET CanvasResolution=? WHERE MainId=?",
                    (float(dpi), canvas_id))

    for (mipmap_id,) in cur.execute("SELECT MainId FROM Mipmap").fetchall():
        _retarget_chain(cur, canvas_id, mipmap_id, levels)

    # サムネイルは寸法固定。中身だけ空にする
    for main_id, attr in cur.execute(
            "SELECT o.MainId, o.Attribute FROM Offscreen o"
            " JOIN LayerThumbnail t ON t.ThumbnailOffscreen = o.MainId").fetchall():
        w, h, _c, _r = attribute_dims(bytes(attr))
        cur.execute("UPDATE Offscreen SET Attribute=? WHERE MainId=?",
                    (retarget_attribute(bytes(attr), w, h), main_id))

    db.commit()
    return levels


def _retarget_chain(cur, canvas_id, mipmap_id, levels):
    """1 本のミップ連鎖を `levels` の段数・寸法へ合わせる。"""
    from clip_write import _next_id, _copy_row, _new_external_id

    node = cur.execute("SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                       (mipmap_id,)).fetchone()[0]
    chain = []
    while node:
        info, offs, nxt = cur.execute(
            "SELECT MainId, Offscreen, NextIndex FROM MipmapInfo WHERE MainId=?",
            (node,)).fetchone()
        chain.append((info, offs))
        node = nxt
    if not chain:
        return

    layer_id = cur.execute("SELECT LayerId FROM MipmapInfo WHERE MainId=?",
                           (chain[0][0],)).fetchone()[0]

    while len(chain) > len(levels):                 # 余った段を消す
        info, offs = chain.pop()
        cur.execute("DELETE FROM MipmapInfo WHERE MainId=?", (info,))
        cur.execute("DELETE FROM Offscreen WHERE MainId=?", (offs,))
    while len(chain) < len(levels):                 # 足りない段は末尾を複製
        info, offs = chain[-1]
        new_off, new_info = _next_id(cur, "Offscreen"), _next_id(cur, "MipmapInfo")
        _copy_row(cur, "Offscreen", "MainId", offs,
                  {"MainId": new_off, "BlockData": _new_external_id()})
        _copy_row(cur, "MipmapInfo", "MainId", info,
                  {"MainId": new_info, "Offscreen": new_off, "NextIndex": 0})
        chain.append((new_info, new_off))

    for i, ((info, offs), (w, h)) in enumerate(zip(chain, levels)):
        scale = 100.0 / (1 << i)
        nxt = chain[i + 1][0] if i + 1 < len(chain) else 0
        cur.execute("UPDATE MipmapInfo SET ThisScale=?, NextIndex=?, CanvasId=?,"
                    " LayerId=? WHERE MainId=?",
                    (scale, nxt, canvas_id, layer_id, info))
        attr = bytes(cur.execute("SELECT Attribute FROM Offscreen WHERE MainId=?",
                                 (offs,)).fetchone()[0])
        cur.execute("UPDATE Offscreen SET Attribute=?, CanvasId=?, LayerId=?"
                    " WHERE MainId=?",
                    (retarget_attribute(attr, w, h), canvas_id, layer_id, offs))
    # **`MipmapCount` を必ず段数に合わせる**。ここを放置すると、CSP は
    # その数だけ連鎖を辿ろうとして**存在しない段まで進み、読み込み中に落ちる**
    # [実測: 段数を減らしたファイルだけ CSP が落ちた]。
    # こちらのリーダは NextIndex=0 で止まるので気付けない。
    cur.execute("UPDATE Mipmap SET BaseMipmapInfo=?, MipmapCount=?, CanvasId=?,"
                " LayerId=? WHERE MainId=?",
                (chain[0][0], len(chain), canvas_id, layer_id, mipmap_id))


def set_canvas_preview(db, rgba):
    """`CanvasPreview` を差し替える。

    キャンバス全体の PNG (RGBA) を持つ 1 行のテーブル [実測: 全サンプルで 1 行]。
    **CSP は開いた直後にここを表示する**ので、雛形のものを残すと
    「起動直後だけ違う絵 (雛形の白いキャンバス) が出る」ことになる
    [実測: WRITE_TEST_4 ④]。レイヤを操作すると再合成されて正しくなる。

    `rgba` は `(h, w, 4)` の uint8。
    """
    import io

    from PIL import Image

    h, w = rgba.shape[:2]
    buf = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, "PNG")
    cur = db.cursor()
    row = cur.execute("SELECT MainId FROM CanvasPreview").fetchone()
    canvas_id = cur.execute("SELECT MainId FROM Canvas").fetchone()[0]
    if row:
        cur.execute("UPDATE CanvasPreview SET ImageType=1, ImageWidth=?,"
                    " ImageHeight=?, ImageData=? WHERE MainId=?",
                    (w, h, buf.getvalue(), row[0]))
    else:
        cur.execute("INSERT INTO CanvasPreview (MainId, CanvasId, ImageType,"
                    " ImageWidth, ImageHeight, ImageData) VALUES (1, ?, 1, ?, ?, ?)",
                    (canvas_id, w, h, buf.getvalue()))
    db.commit()
