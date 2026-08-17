"""CLIP のピクセルブロックを**書く**側 (W2 の中身)。

読む側 (`clip_lazy_demo.decode_block`) の逆写像。`clip_write.py` から使う。

    from clip_encode import build_chunk_payload, patch_block_sizes

**`BlockCheckSum` は 0 を書けばよい** [実測: CSP 5.0.4]。算法は未特定のままだが、
CSP に 3 通り開かせて分かったこと:

    0            → 正常に開く
    CRC32 (別物) → 「レイヤ画像またはレイヤーマスクが破損しています」
    欄ごと省略   → 正常に開く

つまり **CSP は非ゼロの検査値を実際に照合していて、0 は「検査値なし」扱い**。
`--checksum crc32` は切り分け用に残してあるが**使ってはいけない**。

依存: numpy。
"""

import struct
import zlib

import numpy as np

BLOCK_BEGIN = "BlockDataBeginChunk".encode("utf-16be")
BLOCK_END = "BlockDataEndChunk".encode("utf-16be")
ALPHA_TILE = 64
EMPTY_RECORD_SIZE = 104          # 4+4+38 + 20 + 4+34


def encode_rgba_block(rgba, bw, bh):
    """RGBA (bh, bw, 4) を展開後のブロック 1 枚分のバイト列へ。

    decode の逆写像 (docs/CLIP_FORMAT.md §5):

        rows[64:]   … B, G, R, (第 4 チャンネルは復号側が無視する)
        rows[0:64]  … 4x4 スーパーピクセルに畳んだアルファ面を幅 64 の 4 タイルへ
                      alpha[y][x] = tile[y//4][64*(y%4) + x//4][x%4]
    """
    out = np.zeros((bh + ALPHA_TILE, bw, 4), np.uint8)
    color = out[ALPHA_TILE:]
    color[..., 0] = rgba[..., 2]        # B
    color[..., 1] = rgba[..., 1]        # G
    color[..., 2] = rgba[..., 0]        # R
    color[..., 3] = rgba[..., 3]        # 復号側は使わないが CSP もここを書く

    # alpha[y][x] -> out[y//4][64*(y%4) + x//4][x%4]
    a = rgba[..., 3].reshape(ALPHA_TILE, 4, ALPHA_TILE, 4)   # (r, i, c, j)
    for i in range(4):
        out[0:ALPHA_TILE, ALPHA_TILE * i:ALPHA_TILE * (i + 1), :] = a[:, i, :, :]
    return out.tobytes()


def decode_rgba_block(buf, bw, bh):
    """encode_rgba_block の逆 (自己検証用)。"""
    img = np.frombuffer(buf, np.uint8).reshape(bh + ALPHA_TILE, bw, 4)
    color = img[ALPHA_TILE:].copy()
    tiles = [img[0:ALPHA_TILE, ALPHA_TILE * k:ALPHA_TILE * (k + 1)] for k in range(4)]
    alpha = (np.concatenate(tiles, axis=-1)
             .reshape(ALPHA_TILE, ALPHA_TILE, 4, 4)
             .swapaxes(1, 2).reshape(bh, bw))
    color[..., 3] = alpha
    color[:, :, [0, 2]] = color[:, :, [2, 0]]
    return color


def build_block_record(index, raw, bw, bh):
    """ブロック 1 枚のサブレコード。raw=None なら空ブロック (104 バイト固定)。"""
    declen = (bh + ALPHA_TILE) * bw * 4
    tail = struct.pack(">I", 17) + BLOCK_END
    if raw is None:
        head = struct.pack(">II", EMPTY_RECORD_SIZE, 19) + BLOCK_BEGIN
        return head + struct.pack(">5I", index, declen, bw, bh, 0) + tail
    comp = zlib.compress(raw, 9)
    head = struct.pack(">II", len(comp) + 112, 19) + BLOCK_BEGIN
    body = struct.pack(">5I", index, declen, bw, bh, 1)
    # BE の section_size は「続くバイト数 (LE の長さ欄を含む)」、その次が LE の圧縮長
    mid = struct.pack(">I", len(comp) + 4) + struct.pack("<I", len(comp))
    return head + body + mid + comp + tail


def build_trailers(statuses, checksums):
    """末尾の BlockStatus / BlockCheckSum。**サイズ前置が無い**のがブロックとの違い。"""
    out = b""
    for name, values in (("BlockStatus", statuses), ("BlockCheckSum", checksums)):
        out += struct.pack(">I", len(name)) + name.encode("utf-16be")
        out += struct.pack(">III", 12, len(values), 4)
        out += struct.pack(f">{len(values)}I", *values)
    return out


def build_chunk_payload(rgba, attr, checksum="zero"):
    """キャンバス全面の RGBA から CHNKExta のペイロードを組み立てる。

    戻り値は (payload, block_sizes)。`block_sizes` は Attribute へ書き戻す。
    """
    bw, bh = attr["block_width"], attr["block_height"]
    cols, rows = attr["cols"], attr["rows"]
    payload = b""
    sizes, statuses, checksums = [], [], []
    for bi in range(cols * rows):
        br, bc = divmod(bi, cols)
        tile = np.zeros((bh, bw, 4), np.uint8)
        chunk = rgba[br * bh:(br + 1) * bh, bc * bw:(bc + 1) * bw]
        tile[:chunk.shape[0], :chunk.shape[1]] = chunk
        if not tile[..., 3].any():
            rec = build_block_record(bi, None, bw, bh)      # CSP も空レコードを書く
            checksums.append(0)
        else:
            raw = encode_rgba_block(tile, bw, bh)
            rec = build_block_record(bi, raw, bw, bh)
            # 既定は 0 = 「検査値なし」。crc32 は算法違いなので CSP に拒否される
            checksums.append(0 if checksum == "zero"
                             else zlib.crc32(raw) & 0xFFFFFFFF)
        statuses.append(1)
        sizes.append(len(rec))
        payload += rec
    if checksum != "none":
        payload += build_trailers(statuses, checksums)
    return payload, sizes


def patch_block_sizes(attr, sizes):
    """Attribute BLOB の BlockSize 配列を差し替える (セクション長は変わらない)。"""
    marker = "BlockSize".encode("utf-16be")
    i = attr.index(marker) + len(marker)
    magic, nblocks, nchan = struct.unpack_from(">III", attr, i)
    if nblocks != len(sizes):
        raise ValueError(f"ブロック数が合わない: {nblocks} vs {len(sizes)}")
    return attr[:i + 12] + struct.pack(f">{nblocks}I", *sizes)


def parse_attr(attr):
    """Attribute から書き込みに必要な範囲だけ取り出す。"""
    out = {}
    p = 16
    for name in ("Parameter", "InitColor", "BlockSize"):
        p += 4
        if attr[p:p + 18].decode("utf-16be") != name:
            raise ValueError(f"missing {name} section")
        p += 18
        if name == "Parameter":
            v = struct.unpack_from(">20I", attr, p)
            p += 80
            out.update(width=v[0], height=v[1], cols=v[2], rows=v[3],
                       color_mode=v[4], num_channels=v[6], bit_depth=v[7],
                       block_height=v[14], block_width=v[15])
        elif name == "InitColor":
            p += 4
            q = struct.unpack_from(">IIII", attr, p)
            p += 16 + 4 * q[2]
            out["has_init_color"] = bool(q[0])
        else:
            _m, nb, _n = struct.unpack_from(">III", attr, p)
            p += 12
            out["block_sizes"] = list(struct.unpack_from(f">{nb}I", attr, p))
    return out
