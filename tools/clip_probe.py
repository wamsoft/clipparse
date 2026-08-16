"""CLIP ファイルの構造ダンプ (仕様検証用)。

    python tools/clip_probe.py path/to/file.clip [--blocks]

チャンク配置 / SQLite テーブル一覧 / external_id の解決 / Layer ツリー /
Offscreen.Attribute の全フィールド / ブロックサブレコードの実レイアウトを出す。
docs/CLIP_FORMAT.md の記述はこのスクリプトの出力で裏を取っている。

標準ライブラリのみ (struct / sqlite3 / zlib)。
"""

import argparse
import sqlite3
import struct
import sys
import tempfile
import zlib

BLOCK_BEGIN = "BlockDataBeginChunk".encode("utf-16be")


def as_str(v):
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else str(v)


def walk_chunks(raw):
    """先頭から全チャンクを歩く。ペイロードは読まない。"""
    magic, filesize, hdrlen = struct.unpack_from(">8sQQ", raw, 0)
    if magic != b"CSFCHUNK":
        raise ValueError(f"not a .clip file (magic={magic!r})")
    yield ("CSFCHUNK", 0, hdrlen, {"filesize": filesize, "header_len": hdrlen})

    pos = hdrlen
    while pos < len(raw):
        ctype, clen = struct.unpack_from(">8sQ", raw, pos)
        body = pos + 16
        if ctype == b"CHNKHead":
            ver, secsize, idlen = struct.unpack_from(">QQQ", raw, body)
            yield ("CHNKHead", pos, clen, {
                "version": ver,
                "binary_section_size": secsize,
                "identifier": bytes(raw[body + 24: body + 24 + idlen]).hex(),
            })
            pos = body + clen
        elif ctype == b"CHNKExta":
            idlen, extid, dsize = struct.unpack_from(">Q40sQ", raw, body)
            yield ("CHNKExta", pos, clen, {
                "external_id": extid.decode("ascii"),
                "data_size": dsize,
                "data_offset": body + 56,
            })
            pos = body + 56 + dsize
        elif ctype == b"CHNKSQLi":
            yield ("CHNKSQLi", pos, clen, {"db_offset": body, "db_size": clen})
            pos = body + clen
        elif ctype == b"CHNKFoot":
            yield ("CHNKFoot", pos, clen, {})
            break
        else:
            raise ValueError(f"unknown chunk {ctype!r} at {pos}")


def open_db(raw, off, size):
    """SQLite 領域を一時ファイル経由で開く (C++ 版は sqlite3_deserialize を想定)。"""
    f = tempfile.NamedTemporaryFile(delete=False, suffix=".sqlite")
    f.write(raw[off: off + size])
    f.close()
    return sqlite3.connect(f.name), f.name


def parse_attribute(a):
    """Offscreen.Attribute BLOB を全フィールド展開する。"""
    out = {"section_sizes": struct.unpack_from(">IIII", a, 0), "blob_len": len(a)}
    p = 16
    for name in ("Parameter", "InitColor", "BlockSize"):
        boundary = struct.unpack_from(">I", a, p)[0]
        p += 4
        got = a[p:p + 18].decode("utf-16be")
        if got != name:
            raise ValueError(f"expected section {name!r}, got {got!r}")
        p += 18
        out[f"{name}_boundary"] = boundary
        if name == "Parameter":
            v = struct.unpack_from(">20I", a, p)
            p += 80
            out.update(width=v[0], height=v[1], cols=v[2], rows=v[3],
                       color_mode=v[4], alpha_flag=v[5], num_channels=v[6],
                       bit_depth_enum=v[7], block_geom=v[8:12],
                       block_width=v[12], block_height=v[14], block_stride=v[15],
                       subblock=v[16:20])
        elif name == "InitColor":
            out["initcolor_magic"] = struct.unpack_from(">I", a, p)[0]
            p += 4
            q = struct.unpack_from(">IIII", a, p)
            p += 16
            out["has_init_color"] = bool(q[0])
            out["init_color"] = q[1]
            # 追加のチャンネル別初期値は q[2] 個の u32。has_init_color だけを見て
            # 16 バイト固定で読むと num_channels=0 の面 (マスク等) で破綻する。
            out["init_color_count"] = q[2]
            out["init_color_channels"] = q[3]
            if q[2]:
                out["init_color_extra"] = struct.unpack_from(f">{q[2]}I", a, p)
                p += 4 * q[2]
        else:
            magic, nblocks, nchan = struct.unpack_from(">III", a, p)
            p += 12
            out["blocksize_magic"] = magic
            out["block_sizes"] = list(struct.unpack_from(f">{nblocks}I", a, p))
            p += 4 * nblocks
    out["consumed"] = p
    return out


def walk_block_stream(raw, data_off, data_size):
    """CHNKExta ペイロードのサブレコードを歩く。

    ブロックレコードは record_size 前置、末尾の BlockStatus / BlockCheckSum は
    前置なし (name_length から始まる) — その差を実際に確認する。
    """
    p = 0
    while p < data_size:
        first, second = struct.unpack_from(">II", raw, data_off + p)
        if second == struct.unpack_from(">I", BLOCK_BEGIN, 0)[0]:
            # 前置サイズなし: first が name_length
            namelen, has_size = first, False
            name_at = data_off + p + 4
        else:
            namelen, has_size = second, True
            name_at = data_off + p + 8
        name = bytes(raw[name_at: name_at + namelen * 2]).decode("utf-16be")
        body = name_at + namelen * 2

        if name == "BlockDataBeginChunk":
            idx, declen, bw, bh, has = struct.unpack_from(">5I", raw, body)
            rec = {"kind": name, "rel_offset": p, "record_size": first if has_size else None,
                   "block_index": idx, "decompressed_size": declen,
                   "block_w": bw, "block_h": bh, "has_content": has}
            if has:
                rec["section_size"] = struct.unpack_from(">I", raw, body + 20)[0]
                rec["compressed_size"] = struct.unpack_from("<I", raw, body + 24)[0]
                rec["payload_offset"] = body + 28
            yield rec
            p += first if has_size else 0
            if not has_size:
                raise ValueError("BlockDataBeginChunk without size prefix")
        elif name in ("BlockStatus", "BlockCheckSum"):
            hdr, count, width = struct.unpack_from(">III", raw, body)
            entries = struct.unpack_from(f">{count}I", raw, body + 12)
            yield {"kind": name, "rel_offset": p, "header": hdr,
                   "count": count, "entry_width": width, "entries": list(entries)}
            p = (body + 12 + 4 * count) - data_off
        else:
            yield {"kind": "UNKNOWN", "rel_offset": p, "name": name}
            return


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--blocks", action="store_true",
                    help="実体を持つ external チャンクのブロック列も展開する")
    args = ap.parse_args()

    raw = open(args.path, "rb").read()
    print(f"# {args.path}  ({len(raw)} bytes)\n")

    print("## chunks")
    sqlite_off = sqlite_len = None
    externals = {}
    for kind, off, clen, info in walk_chunks(raw):
        print(f"  @{off:<10} {kind:9} len={clen:<10} {info}")
        if kind == "CHNKSQLi":
            sqlite_off, sqlite_len = info["db_offset"], info["db_size"]
        elif kind == "CHNKExta":
            externals[info["external_id"]] = (info["data_offset"], info["data_size"])

    con, tmp = open_db(raw, sqlite_off, sqlite_len)
    cur = con.cursor()

    print("\n## tables")
    tables = [r[0] for r in cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
    for t in tables:
        n = cur.execute(f"SELECT COUNT(*) FROM [{t}]").fetchone()[0]
        print(f"  {t:34} rows={n}")

    print("\n## ExternalTableAndColumnName")
    for tn, cn in cur.execute("SELECT TableName, ColumnName FROM ExternalTableAndColumnName"):
        mark = "" if tn in tables else "   <- table missing in this file"
        print(f"  {tn}.{cn}{mark}")

    print("\n## ExternalChunk (SQLite が持つ絶対オフセット表)")
    ext_off = {}
    for eid, off in cur.execute("SELECT ExternalID, Offset FROM ExternalChunk"):
        eid = as_str(eid)
        ext_off[eid] = off
        hdr = bytes(raw[off:off + 8])
        print(f"  {eid} offset={off:<10} bytes@offset={hdr!r}"
              f"{'  OK' if hdr == b'CHNKExta' else '  MISMATCH'}")
    print(f"  ({len(ext_off)} rows / {len(externals)} CHNKExta chunks in the binary region)")

    print("\n## Canvas")
    cur.execute("SELECT * FROM Canvas")
    cols = [d[0] for d in cur.description]
    row = cur.fetchone()
    for c, v in zip(cols, row):
        if v not in (None, "", 0):
            print(f"  {c} = {v if not isinstance(v, bytes) else f'<{len(v)}B>'}")

    print("\n## Layer tree")
    layers = {}
    for r in cur.execute("SELECT MainId,LayerName,LayerType,LayerFolder,LayerVisibility,"
                         "LayerComposite,LayerOpacity,LayerClip,LayerFirstChildIndex,"
                         "LayerNextIndex,LayerOffsetX,LayerOffsetY FROM Layer"):
        layers[r[0]] = r
    root = cur.execute("SELECT CanvasRootFolder FROM Canvas").fetchone()[0]

    def dump(mid, depth):
        r = layers[mid]
        print(f"  {'  ' * depth}#{r[0]} {r[1]!r:20} type={r[2]:<5} folder={r[3]} "
              f"vis={r[4]} comp={r[5]} opa={r[6]}/256 clip={r[7]} off=({r[10]},{r[11]})")
        child = r[8]
        while child:
            dump(child, depth + 1)
            child = layers[child][9]

    dump(root, 0)

    print("\n## Offscreen")
    mipmap = {}
    for lid, scale, offs in cur.execute(
            "SELECT LayerId, ThisScale, Offscreen FROM MipmapInfo"):
        mipmap[offs] = (lid, scale)
    for mid, lid, attr, bd in cur.execute(
            "SELECT MainId, LayerId, Attribute, BlockData FROM Offscreen"):
        a = parse_attribute(bytes(attr))
        key = as_str(bd)
        role = f"mip {mipmap[mid][1]}%" if mid in mipmap else "thumbnail (no mip)"
        print(f"  #{mid} layer={lid} {role:18} {a['width']}x{a['height']} "
              f"grid={a['cols']}x{a['rows']} nch={a['num_channels']} "
              f"init={'#%08X' % a['init_color'] if a['has_init_color'] else '-'} "
              f"data={'YES' if key in ext_off else 'no'}")
        print(f"      sections={a['section_sizes']} block={a['block_width']}x{a['block_height']} "
              f"sizes={a['block_sizes'][:8]}{'...' if len(a['block_sizes']) > 8 else ''}")

        if args.blocks and key in ext_off:
            data_off, data_size = externals[key]
            total = 0
            for rec in walk_block_stream(raw, data_off, data_size):
                if rec["kind"] == "BlockDataBeginChunk":
                    total += rec["record_size"]
                    ok = (rec["record_size"] == a["block_sizes"][rec["block_index"]])
                    extra = ""
                    if rec["has_content"]:
                        dec = zlib.decompress(
                            raw[rec["payload_offset"]:
                                rec["payload_offset"] + rec["compressed_size"]])
                        extra = (f" comp={rec['compressed_size']} dec={len(dec)}"
                                 f"{' OK' if len(dec) == rec['decompressed_size'] else ' BAD'}")
                    print(f"      block {rec['block_index']:>3} @{rec['rel_offset']:<8} "
                          f"size={rec['record_size']:<7}"
                          f"{'==attr' if ok else '!=attr'} has={rec['has_content']}{extra}")
                else:
                    print(f"      {rec['kind']} count={rec.get('count')} "
                          f"header={rec.get('header')} width={rec.get('entry_width')}")
            print(f"      sum(record_size)={total} data_size={data_size} "
                  f"tail={data_size - total}")

    print(f"\n(temp sqlite: {tmp})")


if __name__ == "__main__":
    sys.exit(main())
