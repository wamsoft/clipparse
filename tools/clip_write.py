"""`.clip` の書き出し (W0/W1 段階の土台)。

    python tools/clip_write.py roundtrip  IN.clip OUT.clip
    python tools/clip_write.py set        IN.clip OUT.clip --layer 5 --opacity 64                                           --name 新しい名前 --visible 0 --composite 2
    python tools/clip_write.py opacity    IN.clip OUT.clip --layer 5 --value 64
    python tools/clip_write.py nothumb    IN.clip OUT.clip
    python tools/clip_write.py verify     A.clip B.clip

**設計上の要点**: `CHNKSQLi` はバイナリ領域の**後ろ**にあるので、
SQLite だけを書き換える編集ではどの `ExternalChunk.Offset` も動かない。
先頭の総サイズと `CHNKSQLi` のチャンク長だけ変わる。
チャンクを増減する編集では、新しいオフセットを先に全部計算してから
`ExternalChunk` を UPDATE し、最後に SQLite を書き出す。

依存: 標準ライブラリのみ (`setpixels` だけ numpy / Pillow / clip_encode)。
"""

import argparse
import hashlib
import os
import sqlite3
import struct
import sys
import tempfile


def as_str(v):
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else str(v)


class ClipFile:
    """チャンクを分解して保持する。SQLite は一時ファイルに出して編集可能にする。"""

    def __init__(self, path):
        self.raw = open(path, "rb").read()
        magic, filesize, hdrlen = struct.unpack_from(">8sQQ", self.raw, 0)
        if magic != b"CSFCHUNK":
            raise ValueError(f"not a .clip file ({magic!r})")
        if filesize != len(self.raw):
            print(f"  警告: ヘッダのファイルサイズ {filesize} != 実サイズ {len(self.raw)}")
        self.header_len = hdrlen

        self.head_body = None           # CHNKHead の本体 40 バイト
        self.externals = []             # [(external_id: bytes40, payload: bytes)]
        self.sqlite_bytes = None
        self.foot_len = 0

        pos = hdrlen
        while pos < len(self.raw):
            ctype, clen = struct.unpack_from(">8sQ", self.raw, pos)
            body = pos + 16
            if ctype == b"CHNKHead":
                self.head_body = self.raw[body:body + clen]
                pos = body + clen
            elif ctype == b"CHNKExta":
                _idlen, extid, dsize = struct.unpack_from(">Q40sQ", self.raw, body)
                self.externals.append((extid, self.raw[body + 56: body + 56 + dsize]))
                pos = body + 56 + dsize
            elif ctype == b"CHNKSQLi":
                self.sqlite_bytes = self.raw[body:body + clen]
                pos = body + clen
            elif ctype == b"CHNKFoot":
                self.foot_len = clen
                break
            else:
                raise ValueError(f"unknown chunk {ctype!r} @{pos}")

        f = tempfile.NamedTemporaryFile(delete=False, suffix=".sqlite")
        f.write(self.sqlite_bytes)
        f.close()
        self.db_path = f.name
        self.db = sqlite3.connect(self.db_path)

    def close(self):
        self.db.close()
        try:
            os.unlink(self.db_path)
        except OSError:
            pass

    # --- 書き出し -------------------------------------------------------

    def save(self, dst):
        """現在の externals と SQLite でファイルを組み立てる。

        オフセットは先に全部確定させ、`ExternalChunk` を更新してから
        SQLite を直列化する (更新後に位置が動かないのがミソ)。
        """
        # 1) 新しいオフセットを計算する (CHNKExta ヘッダ先頭の絶対位置)
        pos = self.header_len + 16 + len(self.head_body)
        offsets = {}
        for extid, payload in self.externals:
            offsets[as_str(extid)] = pos
            pos += 16 + 56 + len(payload)
        binary_section_size = pos          # = CHNKSQLi チャンクヘッダの位置

        # 2) ExternalChunk を更新 (存在しない external_id の行は消す)
        #
        # 注意: `ExternalChunk.ExternalID` は **BLOB 宣言だが値は TEXT** で入っている
        # [実測: typeof() が 'text']。bytes を束縛すると 1 行もマッチせず、
        # 更新も削除も黙って失敗する。CSP の格納型に合わせて str を束縛すること。
        cur = self.db.cursor()
        have = {as_str(e) for e, in cur.execute("SELECT ExternalID FROM ExternalChunk")}
        for eid, off in offsets.items():
            if eid in have:
                cur.execute("UPDATE ExternalChunk SET Offset=? WHERE ExternalID=?", (off, eid))
            else:
                cur.execute("INSERT INTO ExternalChunk (ExternalID, Offset) VALUES (?, ?)",
                            (eid, off))
        for eid in have - set(offsets):
            cur.execute("DELETE FROM ExternalChunk WHERE ExternalID=?", (eid,))
        self.db.commit()
        n_rows = cur.execute("SELECT COUNT(*) FROM ExternalChunk").fetchone()[0]
        if n_rows != len(offsets):
            raise RuntimeError(
                f"ExternalChunk の行数 {n_rows} が外部チャンク数 {len(offsets)} と合わない")

        # 3) SQLite を読み出す。VACUUM せずそのまま使う (無変更なら元と同一バイト)
        db_bytes = open(self.db_path, "rb").read()

        # 4) CHNKHead の binary_section_size を差し替える
        head = bytearray(self.head_body)
        struct.pack_into(">Q", head, 8, binary_section_size)

        out = bytearray()
        out += struct.pack(">8sQQ", b"CSFCHUNK", 0, self.header_len)   # サイズは後で埋める
        out += struct.pack(">8sQ", b"CHNKHead", len(head)) + head
        for extid, payload in self.externals:
            out += struct.pack(">8sQ", b"CHNKExta", 56 + len(payload))
            out += struct.pack(">Q40sQ", 40, extid, len(payload))
            out += payload
        assert len(out) == binary_section_size, (len(out), binary_section_size)
        out += struct.pack(">8sQ", b"CHNKSQLi", len(db_bytes)) + db_bytes
        out += struct.pack(">8sQ", b"CHNKFoot", self.foot_len)
        struct.pack_into(">Q", out, 8, len(out))

        with open(dst, "wb") as f:
            f.write(out)
        return len(out)


def cmd_roundtrip(args):
    """無変更で読み書きし、元とバイト一致するか確かめる。"""
    c = ClipFile(args.src)
    n = c.save(args.dst)
    c.close()
    a = hashlib.sha256(open(args.src, "rb").read()).hexdigest()
    b = hashlib.sha256(open(args.dst, "rb").read()).hexdigest()
    print(f"  {args.src}  {os.path.getsize(args.src):,} B  sha256={a[:16]}")
    print(f"  {args.dst}  {n:,} B  sha256={b[:16]}")
    if a == b:
        print("  → バイト完全一致")
        return 0
    ra, rb = open(args.src, "rb").read(), open(args.dst, "rb").read()
    if len(ra) != len(rb):
        print(f"  → サイズが違う ({len(ra)} vs {len(rb)})")
    else:
        diff = [i for i in range(len(ra)) if ra[i] != rb[i]]
        print(f"  → {len(diff)} バイト違う。最初の 20 箇所: {diff[:20]}")
    return 1


# W1 (属性編集) で触れる列。いずれも SQLite の UPDATE だけで済み、
# バイナリ領域に触らないので `ExternalChunk.Offset` が動かない。
ATTR_COLUMNS = {
    "name":      ("LayerName",       str),
    "visible":   ("LayerVisibility", int),   # 0 / 1
    "opacity":   ("LayerOpacity",    int),   # 0..256 (**255 ではない**)
    "composite": ("LayerComposite",  int),   # docs/CLIP_FORMAT.md §9
    "clip":      ("LayerClip",       int),   # 0 / 1
    "folder":    ("LayerFolder",     int),   # bit0=フォルダ, bit4=折り畳み
}


def cmd_set(args):
    """レイヤ属性を変更する (W1)。SQLite の UPDATE のみ。"""
    c = ClipFile(args.src)
    cur = c.db.cursor()
    row = cur.execute("SELECT LayerName FROM Layer WHERE MainId=?",
                      (args.layer,)).fetchone()
    if row is None:
        print(f"  レイヤ #{args.layer} が無い")
        c.close()
        return 1

    changed = []
    for key, (column, conv) in ATTR_COLUMNS.items():
        val = getattr(args, key, None)
        if val is None:
            continue
        before = cur.execute(f"SELECT [{column}] FROM Layer WHERE MainId=?",
                             (args.layer,)).fetchone()[0]
        cur.execute(f"UPDATE Layer SET [{column}]=? WHERE MainId=?",
                    (conv(val), args.layer))
        changed.append(f"{column}: {before!r} -> {conv(val)!r}")
    if not changed:
        print("  変更する属性が指定されていない")
        c.close()
        return 1
    c.db.commit()
    n = c.save(args.dst)
    c.close()
    print(f"  レイヤ #{args.layer} {row[0]!r}")
    for line in changed:
        print(f"    {line}")
    print(f"  {args.dst}  {n:,} B")
    return 0


def cmd_setpixels(args):
    """レイヤの 100% ミップの画素を差し替える (W2)。

    チャンクを作り直すので後続チャンクのオフセットが全部ずれる。`save()` が
    `ExternalChunk` を更新する。

    **`BlockCheckSum` の算法が未解読**なので、書き方を 3 通り選べる (`--checksum`)。
    CSP 実機でどれが通るかを切り分けるため。詳細は `tools/clip_encode.py`。
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import numpy as np
    from PIL import Image
    import clip_encode as enc

    c = ClipFile(args.src)
    cur = c.db.cursor()
    row = cur.execute("SELECT LayerName, LayerRenderMipmap FROM Layer WHERE MainId=?",
                      (args.layer,)).fetchone()
    if row is None:
        print(f"  レイヤ #{args.layer} が無い")
        c.close()
        return 1
    base = cur.execute("SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                       (row[1],)).fetchone()
    offs = cur.execute("SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                       (base[0],)).fetchone()[0]
    attr, bdid = cur.execute(
        "SELECT Attribute, BlockData FROM Offscreen WHERE MainId=?", (offs,)).fetchone()
    attr = bytes(attr)
    a = enc.parse_attr(attr)
    if a["num_channels"] != 4:
        print(f"  RGBA 以外の面は未対応 (num_channels={a['num_channels']})")
        c.close()
        return 1

    img = Image.open(args.png).convert("RGBA")
    if img.size != (a["width"], a["height"]):
        print(f"  画像を {img.size} から {(a['width'], a['height'])} へ合わせる")
        img = img.resize((a["width"], a["height"]), Image.LANCZOS)

    payload, sizes = enc.build_chunk_payload(np.array(img), a, args.checksum)
    cur.execute("UPDATE Offscreen SET Attribute=? WHERE MainId=?",
                (enc.patch_block_sizes(attr, sizes), offs))
    c.db.commit()

    key = as_str(bdid)
    for i, (extid, _p) in enumerate(c.externals):
        if as_str(extid) == key:
            c.externals[i] = (extid, payload)
            break
    else:
        c.externals.append((key.encode("ascii"), payload))

    n = c.save(args.dst)
    c.close()
    nonempty = sum(1 for s in sizes if s != enc.EMPTY_RECORD_SIZE)
    print(f"  レイヤ #{args.layer} {row[0]!r}  offscreen #{offs} "
          f"{a['width']}x{a['height']} ({a['cols']}x{a['rows']} ブロック)")
    print(f"    画素ありブロック {nonempty}/{len(sizes)}   "
          f"チャンク {len(payload):,} B   checksum={args.checksum}")
    print(f"  {args.dst}  {n:,} B")
    return 0


def _next_id(cur, table):
    """`ElemScheme.MaxIndex` から MainId を 1 つ払い出す。

    **MainId は SQLite の AUTOINCREMENT ではなく `ElemScheme` が採番元** [実測]。
    水位は削除しても下がらないので、常に +1 して水位を更新する。
    """
    row = cur.execute("SELECT MaxIndex FROM ElemScheme WHERE TableName=?",
                      (table,)).fetchone()
    if row is None:
        row = (cur.execute(f"SELECT COALESCE(MAX(MainId), 0) FROM [{table}]")
                  .fetchone())
    new = int(row[0]) + 1
    cur.execute("UPDATE ElemScheme SET MaxIndex=? WHERE TableName=?", (new, table))
    return new


def _copy_row(cur, table, where_col, where_val, overrides):
    """1 行を複製して、`overrides` の列だけ差し替えて INSERT する。

    **列を列挙せずに丸ごと写す**のがミソ。CSP が期待する既定値が
    57 列 (Layer) / 43 列 (LayerThumbnail) もあり、そのほとんどは意味が
    分かっていない。テンプレート行から引き継げば当てずっぽうを書かずに済む。
    `_PW_ID` は AUTOINCREMENT なので除く。
    """
    cols = [d[1] for d in cur.execute(f"PRAGMA table_info([{table}])")]
    cols = [c for c in cols if c != "_PW_ID"]
    src = cur.execute(f"SELECT {','.join('[' + c + ']' for c in cols)} "
                      f"FROM [{table}] WHERE [{where_col}]=?", (where_val,)).fetchone()
    if src is None:
        raise ValueError(f"{table}.{where_col}={where_val} が無い")
    values = [overrides.get(c, v) for c, v in zip(cols, src)]
    ph = ",".join("?" * len(cols))
    cur.execute(f"INSERT INTO [{table}] ({','.join('[' + c + ']' for c in cols)}) "
                f"VALUES ({ph})", values)


def _new_external_id():
    import uuid
    return "extrnlid" + uuid.uuid4().hex.upper()


def cmd_addlayer(args):
    """既存レイヤを雛形にして新しいレイヤを足す (W3)。

    `Layer` は 57 列、`LayerThumbnail` は 43 列あり、CSP が期待する既定値の
    大半は意味が分かっていない。**列挙せず既存行を丸ごと複製**して、
    ID とリンクと画素だけ差し替える。

    ミップの縮小段とサムネイルには画素を入れない。実測で CSP は
    100% 段とサムネイルにしか画素を書かず、しかもサムネイルは開いた時に
    再生成してくれることを確認済み (docs/WRITE_TEST.md)。
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import numpy as np
    from PIL import Image
    import clip_encode as enc

    c = ClipFile(args.src)
    cur = c.db.cursor()

    src_row = cur.execute(
        "SELECT LayerName, LayerRenderMipmap, LayerRenderThumbnail,"
        " LayerLayerMaskMipmap, LayerFolder FROM Layer WHERE MainId=?",
        (args.copy_from,)).fetchone()
    if src_row is None:
        print(f"  雛形レイヤ #{args.copy_from} が無い")
        c.close()
        return 1
    if src_row[4] & 1:
        print("  フォルダは雛形にできない")
        c.close()
        return 1

    canvas_id, root = cur.execute(
        "SELECT MainId, CanvasRootFolder FROM Canvas").fetchone()

    # --- ID を払い出す ---
    new_layer = _next_id(cur, "Layer")
    new_mipmap = _next_id(cur, "Mipmap")
    new_thumb = _next_id(cur, "LayerThumbnail")

    # --- ミップ連鎖を複製する ---
    chain = []
    node = cur.execute("SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                       (src_row[1],)).fetchone()[0]
    while node:
        scale, offs, nxt = cur.execute(
            "SELECT ThisScale, Offscreen, NextIndex FROM MipmapInfo WHERE MainId=?",
            (node,)).fetchone()
        chain.append((node, scale, offs))
        node = nxt

    new_infos = [_next_id(cur, "MipmapInfo") for _ in chain]
    new_offs = [_next_id(cur, "Offscreen") for _ in chain]
    for i, (old_info, scale, old_off) in enumerate(chain):
        ext_id = _new_external_id()
        _copy_row(cur, "Offscreen", "MainId", old_off,
                  {"MainId": new_offs[i], "LayerId": new_layer,
                   "CanvasId": canvas_id, "BlockData": ext_id})
        _copy_row(cur, "MipmapInfo", "MainId", old_info,
                  {"MainId": new_infos[i], "LayerId": new_layer,
                   "CanvasId": canvas_id, "Offscreen": new_offs[i],
                   "NextIndex": new_infos[i + 1] if i + 1 < len(chain) else 0})
    _copy_row(cur, "Mipmap", "MainId", src_row[1],
              {"MainId": new_mipmap, "LayerId": new_layer, "CanvasId": canvas_id,
               "BaseMipmapInfo": new_infos[0]})

    # --- サムネイル (画素は入れない。CSP が再生成する) ---
    old_tn_off = cur.execute("SELECT ThumbnailOffscreen FROM LayerThumbnail"
                             " WHERE MainId=?", (src_row[2],)).fetchone()[0]
    new_tn_off = _next_id(cur, "Offscreen")
    _copy_row(cur, "Offscreen", "MainId", old_tn_off,
              {"MainId": new_tn_off, "LayerId": new_layer, "CanvasId": canvas_id,
               "BlockData": _new_external_id()})
    _copy_row(cur, "LayerThumbnail", "MainId", src_row[2],
              {"MainId": new_thumb, "LayerId": new_layer, "CanvasId": canvas_id,
               "ThumbnailOffscreen": new_tn_off})

    # --- Layer 行 ---
    import uuid
    _copy_row(cur, "Layer", "MainId", args.copy_from, {
        "MainId": new_layer, "CanvasId": canvas_id,
        "LayerName": args.name, "LayerUuid": str(uuid.uuid4()),
        "LayerFirstChildIndex": 0, "LayerNextIndex": 0,
        "LayerRenderMipmap": new_mipmap, "LayerRenderThumbnail": new_thumb,
        "LayerLayerMaskMipmap": 0, "LayerLayerMaskThumbnail": 0,
        "LayerSelect": 0, "LayerVisibility": 1,
    })

    # --- 兄弟チェーンへ繋ぐ (--after の直上。既定は最上段) ---
    kids = []
    node = cur.execute("SELECT LayerFirstChildIndex FROM Layer WHERE MainId=?",
                       (root,)).fetchone()[0]
    while node:
        kids.append(node)
        node = cur.execute("SELECT LayerNextIndex FROM Layer WHERE MainId=?",
                           (node,)).fetchone()[0]
    after = args.after if args.after is not None else (kids[-1] if kids else 0)
    if after:
        nxt = cur.execute("SELECT LayerNextIndex FROM Layer WHERE MainId=?",
                          (after,)).fetchone()[0]
        cur.execute("UPDATE Layer SET LayerNextIndex=? WHERE MainId=?",
                    (new_layer, after))
        cur.execute("UPDATE Layer SET LayerNextIndex=? WHERE MainId=?",
                    (nxt, new_layer))
    else:
        cur.execute("UPDATE Layer SET LayerFirstChildIndex=? WHERE MainId=?",
                    (new_layer, root))

    # --- 100% ミップの画素 ---
    attr, bdid = cur.execute(
        "SELECT Attribute, BlockData FROM Offscreen WHERE MainId=?",
        (new_offs[0],)).fetchone()
    a = enc.parse_attr(bytes(attr))
    if args.png:
        img = Image.open(args.png).convert("RGBA")
        if img.size != (a["width"], a["height"]):
            img = img.resize((a["width"], a["height"]), Image.LANCZOS)
        rgba = np.array(img)
    else:
        rgba = np.zeros((a["height"], a["width"], 4), np.uint8)
    payload, sizes = enc.build_chunk_payload(rgba, a, args.checksum)
    cur.execute("UPDATE Offscreen SET Attribute=? WHERE MainId=?",
                (enc.patch_block_sizes(bytes(attr), sizes), new_offs[0]))
    c.db.commit()
    c.externals.append((as_str(bdid).encode("ascii"), payload))

    n = c.save(args.dst)
    c.close()
    print(f"  雛形 #{args.copy_from} {src_row[0]!r} -> 新レイヤ #{new_layer} {args.name!r}")
    print(f"    ミップ {len(chain)} 段 (Offscreen {new_offs[0]}..{new_offs[-1]}) "
          f"+ サムネイル #{new_tn_off}")
    print(f"    100% 段に {sum(1 for s in sizes if s != enc.EMPTY_RECORD_SIZE)}"
          f"/{len(sizes)} ブロック  チャンク {len(payload):,} B")
    print(f"  {args.dst}  {n:,} B")
    return 0


def cmd_opacity(args):
    """レイヤ不透明度だけ変更する (SQLite の UPDATE のみ。オフセットは動かない)。"""
    c = ClipFile(args.src)
    cur = c.db.cursor()
    row = cur.execute("SELECT LayerName, LayerOpacity FROM Layer WHERE MainId=?",
                      (args.layer,)).fetchone()
    if row is None:
        print(f"  レイヤ #{args.layer} が無い")
        c.close()
        return 1
    cur.execute("UPDATE Layer SET LayerOpacity=? WHERE MainId=?", (args.value, args.layer))
    c.db.commit()
    n = c.save(args.dst)
    c.close()
    print(f"  レイヤ #{args.layer} {row[0]!r}: 不透明度 {row[1]}/256 -> {args.value}/256 "
          f"({args.value * 100 // 256}%)")
    print(f"  {args.dst}  {n:,} B")
    return 0


def cmd_nothumb(args):
    """全サムネイルのチャンク実体を削る (CSP が再生成するかの確認用)。

    チャンクが減るのでバイナリ領域が詰まり、以降の `ExternalChunk.Offset` が
    全部ずれる。`save()` がその更新をやる。
    """
    c = ClipFile(args.src)
    cur = c.db.cursor()
    thumbs = set()
    for (offs,) in cur.execute("SELECT ThumbnailOffscreen FROM LayerThumbnail"):
        if offs:
            r = cur.execute("SELECT BlockData FROM Offscreen WHERE MainId=?", (offs,)).fetchone()
            if r and r[0]:
                thumbs.add(as_str(r[0]))
    before = len(c.externals)
    c.externals = [(e, p) for e, p in c.externals if as_str(e) not in thumbs]
    n = c.save(args.dst)
    c.close()
    print(f"  サムネイルの external {len(thumbs)} 件のうち、"
          f"実体を持っていた {before - len(c.externals)} 件を削除")
    print(f"  外部チャンク {before} -> {len(c.externals)}")
    print(f"  {args.dst}  {n:,} B  (元 {os.path.getsize(args.src):,} B)")
    return 0


def cmd_verify(args):
    """2 ファイルの構造を比較する (CSP で開いて上書き保存したものの確認用)。"""
    a, b = ClipFile(args.src), ClipFile(args.dst)
    print(f"  外部チャンク数: {len(a.externals)} vs {len(b.externals)}")
    print(f"  SQLite サイズ : {len(a.sqlite_bytes):,} vs {len(b.sqlite_bytes):,}")
    for name, db in (("A", a.db), ("B", b.db)):
        cur = db.cursor()
        n = cur.execute("SELECT COUNT(*) FROM Layer").fetchone()[0]
        ops = cur.execute("SELECT MainId, LayerName, LayerOpacity FROM Layer "
                          "ORDER BY MainId").fetchall()
        print(f"  {name}: Layer {n} 行  " +
              " / ".join(f"#{m}:{o}" for m, _nm, o in ops))
    a.close(); b.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("roundtrip", cmd_roundtrip), ("set", cmd_set),
                     ("setpixels", cmd_setpixels), ("addlayer", cmd_addlayer),
                     ("opacity", cmd_opacity), ("nothumb", cmd_nothumb),
                     ("verify", cmd_verify)):
        p = sub.add_parser(name)
        p.add_argument("src")
        p.add_argument("dst")
        if name == "opacity":
            p.add_argument("--layer", type=int, required=True)
            p.add_argument("--value", type=int, required=True, help="0..256")
        if name == "setpixels":
            p.add_argument("--layer", type=int, required=True, help="Layer.MainId")
            p.add_argument("--png", required=True, help="差し替える画像")
            p.add_argument("--checksum", default="zero",
                           choices=("zero", "crc32", "none"),
                           help="BlockCheckSum の書き方 (算法が未解読なので選べる)")
        if name == "addlayer":
            p.add_argument("--copy-from", type=int, required=True,
                           dest="copy_from", help="雛形にする Layer.MainId")
            p.add_argument("--name", default="new layer")
            p.add_argument("--png", help="入れる画像 (省略で透明)")
            p.add_argument("--after", type=int,
                           help="この MainId の直上へ入れる (既定は最上段)")
            p.add_argument("--checksum", default="zero",
                           choices=("zero", "crc32", "none"))
        if name == "set":
            p.add_argument("--layer", type=int, required=True, help="Layer.MainId")
            p.add_argument("--name", help="レイヤ名")
            p.add_argument("--visible", type=int, choices=(0, 1))
            p.add_argument("--opacity", type=int, help="0..256")
            p.add_argument("--composite", type=int, help="LayerComposite")
            p.add_argument("--clip", type=int, choices=(0, 1), help="クリッピング")
            p.add_argument("--folder", type=int, help="LayerFolder ビット")
        p.set_defaults(func=fn)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
