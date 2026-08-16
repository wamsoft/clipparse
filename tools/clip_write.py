"""`.clip` の書き出し (W0/W1 段階の土台)。

    python tools/clip_write.py roundtrip  IN.clip OUT.clip
    python tools/clip_write.py opacity    IN.clip OUT.clip --layer 5 --value 64
    python tools/clip_write.py nothumb    IN.clip OUT.clip
    python tools/clip_write.py verify     A.clip B.clip

**設計上の要点**: `CHNKSQLi` はバイナリ領域の**後ろ**にあるので、
SQLite だけを書き換える編集ではどの `ExternalChunk.Offset` も動かない。
先頭の総サイズと `CHNKSQLi` のチャンク長だけ変わる。
チャンクを増減する編集では、新しいオフセットを先に全部計算してから
`ExternalChunk` を UPDATE し、最後に SQLite を書き出す。

依存: 標準ライブラリのみ。
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
    for name, fn in (("roundtrip", cmd_roundtrip), ("opacity", cmd_opacity),
                     ("nothumb", cmd_nothumb), ("verify", cmd_verify)):
        p = sub.add_parser(name)
        p.add_argument("src")
        p.add_argument("dst")
        if name == "opacity":
            p.add_argument("--layer", type=int, required=True)
            p.add_argument("--value", type=int, required=True, help="0..256")
        p.set_defaults(func=fn)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
