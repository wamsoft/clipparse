#include "clipencode.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>

namespace clip {

  namespace {
    inline void putBE32(std::vector<uint8_t> &v, uint32_t x) {
      v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
      v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
    }
    inline void putLE32(std::vector<uint8_t> &v, uint32_t x) {
      v.push_back(uint8_t(x));       v.push_back(uint8_t(x >> 8));
      v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
    }
    // ASCII 文字列を UTF-16BE で積む (CLIP のマーカーは全部 ASCII)
    inline void putUtf16be(std::vector<uint8_t> &v, const char *s) {
      for (const char *p = s; *p; ++p) { v.push_back(0); v.push_back(uint8_t(*p)); }
    }
    inline void putBE32At(std::vector<uint8_t> &v, size_t at, uint32_t x) {
      v[at] = uint8_t(x >> 24); v[at + 1] = uint8_t(x >> 16);
      v[at + 2] = uint8_t(x >> 8); v[at + 3] = uint8_t(x);
    }
  }

  // ---- ピクセルブロック ---------------------------------------------------

  void encodeRgbaBlock(const uint8_t *rgba, uint32_t bw, uint32_t bh,
                       std::vector<uint8_t> &out) {
    const uint32_t H = bh + ALPHA_TILE;
    out.assign(size_t(H) * bw * 4, 0);

    // 色面: rows[64:] に B,G,R,A の順で置く (第 4 面は復号側が使わない)
    for (uint32_t y = 0; y < bh; ++y) {
      const uint8_t *src = rgba + size_t(y) * bw * 4;
      uint8_t *dst = out.data() + (size_t(y + ALPHA_TILE) * bw) * 4;
      for (uint32_t x = 0; x < bw; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = src[x * 4 + 3];
      }
    }
    // アルファ面: alpha[y][x] -> out[y/4][64*(y%4) + x/4][x%4]
    // **タイル番号は y から、チャンネルは x から**取る (逆にすると壊れる)。
    for (uint32_t y = 0; y < bh; ++y) {
      const uint8_t *src = rgba + size_t(y) * bw * 4;
      uint8_t *row = out.data() + (size_t(y / 4) * bw) * 4;
      const uint32_t colBase = ALPHA_TILE * (y % 4);
      for (uint32_t x = 0; x < bw; ++x)
        row[(colBase + x / 4) * 4 + (x % 4)] = src[x * 4 + 3];
    }
  }

  void decodeRgbaBlock(const uint8_t *buf, uint32_t bw, uint32_t bh,
                       std::vector<uint8_t> &rgba) {
    rgba.assign(size_t(bw) * bh * 4, 0);
    for (uint32_t y = 0; y < bh; ++y) {
      const uint8_t *src = buf + (size_t(y + ALPHA_TILE) * bw) * 4;
      const uint8_t *arow = buf + (size_t(y / 4) * bw) * 4;
      const uint32_t colBase = ALPHA_TILE * (y % 4);
      uint8_t *dst = rgba.data() + size_t(y) * bw * 4;
      for (uint32_t x = 0; x < bw; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = arow[(colBase + x / 4) * 4 + (x % 4)];
      }
    }
  }

  void buildBlockRecord(uint32_t index, const std::vector<uint8_t> *raw,
                        uint32_t bw, uint32_t bh, std::vector<uint8_t> &out) {
    const uint32_t declen = (bh + ALPHA_TILE) * bw * 4;
    out.clear();
    if (!raw) {
      putBE32(out, EMPTY_RECORD_SIZE);
      putBE32(out, 19);
      putUtf16be(out, "BlockDataBeginChunk");
      putBE32(out, index); putBE32(out, declen);
      putBE32(out, bw);    putBE32(out, bh);
      putBE32(out, 0);
      putBE32(out, 17);
      putUtf16be(out, "BlockDataEndChunk");
      return;
    }
    uLongf clen = compressBound(uLong(raw->size()));
    // `comp(size_t(clen))` と書くと**関数宣言**に取られる (most vexing parse)
    std::vector<uint8_t> comp;
    comp.resize(size_t(clen));
    if (compress2(comp.data(), &clen, raw->data(), uLong(raw->size()), 9) != Z_OK) {
      out.clear();
      return;
    }
    comp.resize(size_t(clen));

    putBE32(out, uint32_t(comp.size()) + 112);      // サブレコード全長
    putBE32(out, 19);
    putUtf16be(out, "BlockDataBeginChunk");
    putBE32(out, index); putBE32(out, declen);
    putBE32(out, bw);    putBE32(out, bh);
    putBE32(out, 1);
    putBE32(out, uint32_t(comp.size()) + 4);        // 続くバイト数 (LE の長さ欄込み)
    putLE32(out, uint32_t(comp.size()));            // **ここだけリトルエンディアン**
    out.insert(out.end(), comp.begin(), comp.end());
    putBE32(out, 17);
    putUtf16be(out, "BlockDataEndChunk");
  }

  void buildTrailers(const std::vector<uint32_t> &status,
                     const std::vector<uint32_t> &checksum,
                     std::vector<uint8_t> &out) {
    const char *names[2] = { "BlockStatus", "BlockCheckSum" };
    const std::vector<uint32_t> *vals[2] = { &status, &checksum };
    for (int k = 0; k < 2; ++k) {
      putBE32(out, uint32_t(strlen(names[k])));
      putUtf16be(out, names[k]);
      putBE32(out, 12);
      putBE32(out, uint32_t(vals[k]->size()));
      putBE32(out, 4);
      for (uint32_t v : *vals[k]) putBE32(out, v);
    }
  }

  bool buildChunkPayload(const uint8_t *rgba, uint32_t w, uint32_t h,
                         const OffscreenAttr &attr,
                         std::vector<uint8_t> &payload,
                         std::vector<uint32_t> &sizes) {
    if (w != attr.width || h != attr.height) return false;
    const uint32_t bw = attr.blockWidth, bh = attr.blockHeight;
    if (!bw || !bh) return false;

    payload.clear();
    sizes.clear();
    std::vector<uint32_t> status, checksum;
    std::vector<uint8_t> tile, raw, rec;
    tile.resize(size_t(bw) * bh * 4);

    for (uint32_t bi = 0; bi < attr.cols * attr.rows; ++bi) {
      const uint32_t br = bi / attr.cols, bc = bi % attr.cols;
      std::fill(tile.begin(), tile.end(), uint8_t(0));
      bool any = false;
      for (uint32_t y = 0; y < bh; ++y) {
        const uint32_t sy = br * bh + y;
        if (sy >= h) break;
        const uint8_t *src = rgba + (size_t(sy) * w + bc * bw) * 4;
        const uint32_t n = std::min(bw, w - bc * bw);
        memcpy(&tile[size_t(y) * bw * 4], src, size_t(n) * 4);
        for (uint32_t x = 0; x < n; ++x)
          if (src[x * 4 + 3]) { any = true; break; }
      }
      if (!any) {
        buildBlockRecord(bi, nullptr, bw, bh, rec);   // CSP も空レコードを書く
      } else {
        encodeRgbaBlock(tile.data(), bw, bh, raw);
        buildBlockRecord(bi, &raw, bw, bh, rec);
        if (rec.empty()) return false;
      }
      // **チェックサムは 0 固定**。非ゼロだと CSP が照合して「破損」になる。
      checksum.push_back(0);
      status.push_back(1);
      sizes.push_back(uint32_t(rec.size()));
      payload.insert(payload.end(), rec.begin(), rec.end());
    }
    buildTrailers(status, checksum, payload);
    return true;
  }

  // ---- Attribute ----------------------------------------------------------

  namespace {
    // セクション名 (UTF-16BE 18 バイト) を探す
    int findMarker(const uint8_t *a, int len, const char *name) {
      const size_t n = strlen(name);
      std::vector<uint8_t> pat;
      putUtf16be(pat, name);
      for (int i = 0; i + int(pat.size()) <= len; ++i)
        if (memcmp(a + i, pat.data(), pat.size()) == 0) return i + int(n * 2);
      return -1;
    }
  }

  bool patchBlockSizes(std::vector<uint8_t> &attr,
                       const std::vector<uint32_t> &sizes) {
    const int p = findMarker(attr.data(), int(attr.size()), "BlockSize");
    if (p < 0 || p + 12 > int(attr.size())) return false;
    const uint32_t nblocks = beU32(attr.data() + p + 4);
    if (nblocks != sizes.size()) return false;
    if (p + 12 + int(nblocks) * 4 > int(attr.size())) return false;
    for (uint32_t i = 0; i < nblocks; ++i)
      putBE32At(attr, size_t(p) + 12 + size_t(i) * 4, sizes[i]);
    attr.resize(size_t(p) + 12 + size_t(nblocks) * 4);
    return true;
  }

  static bool splitAttribute(const uint8_t *a, int len,
                             std::vector<std::vector<uint8_t>> &sec) {
    if (len < 16) return false;
    uint32_t sizes[4];
    for (int i = 0; i < 4; ++i) sizes[i] = beU32(a + i * 4);
    int p = int(sizes[0]);
    for (int i = 1; i < 4; ++i) {
      if (p + int(sizes[i]) > len) return false;
      sec.emplace_back(a + p, a + p + sizes[i]);
      p += int(sizes[i]);
    }
    return p == len;
  }

  bool retargetAttribute(const uint8_t *attr, int len, uint32_t w, uint32_t h,
                         const std::vector<uint32_t> *sizes,
                         std::vector<uint8_t> &out) {
    std::vector<std::vector<uint8_t>> sec;
    if (!splitAttribute(attr, len, sec) || sec.size() != 3) return false;

    const uint32_t cols = (w + 255) / 256, rows = (h + 255) / 256;
    const uint32_t nblocks = cols * rows;
    std::vector<uint32_t> use;
    if (sizes) {
      if (sizes->size() != nblocks) return false;
      use = *sizes;
    } else {
      use.assign(nblocks, EMPTY_RECORD_SIZE);
    }

    std::vector<uint8_t> param = sec[0];
    if (param.size() < 22 + 16) return false;
    putBE32At(param, 22, w);      putBE32At(param, 26, h);
    putBE32At(param, 30, cols);   putBE32At(param, 34, rows);

    if (sec[2].size() < 22 + 12) return false;
    const uint32_t nchan = beU32(sec[2].data() + 30);      // 既存値を保つ
    std::vector<uint8_t> blk(sec[2].begin(), sec[2].begin() + 22);
    putBE32(blk, 12); putBE32(blk, nblocks); putBE32(blk, nchan);
    for (uint32_t s : use) putBE32(blk, s);

    out.clear();
    putBE32(out, 16);
    putBE32(out, uint32_t(param.size()));
    putBE32(out, uint32_t(sec[1].size()));
    putBE32(out, uint32_t(blk.size()));
    out.insert(out.end(), param.begin(), param.end());
    out.insert(out.end(), sec[1].begin(), sec[1].end());
    out.insert(out.end(), blk.begin(), blk.end());
    return true;
  }

  std::vector<std::pair<uint32_t, uint32_t>> mipLevels(uint32_t w, uint32_t h) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    auto isOne = [](uint32_t a, uint32_t b) {
      return (a + 255) / 256 == 1 && (b + 255) / 256 == 1;
    };
    for (;;) {
      out.emplace_back(w, h);
      const size_t n = out.size();
      if (n >= 2 && isOne(out[n - 1].first, out[n - 1].second) &&
          isOne(out[n - 2].first, out[n - 2].second))
        return out;
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
    }
  }

  // ---- PNG ----------------------------------------------------------------

  static void pngChunk(std::vector<uint8_t> &png, const char *type,
                       const uint8_t *data, size_t len) {
    putBE32(png, uint32_t(len));
    const size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    if (len) png.insert(png.end(), data, data + len);
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), png.data() + at,
                            uInt(4 + len));
    putBE32(png, uint32_t(crc));
  }

  bool encodePng(const uint8_t *rgba, uint32_t w, uint32_t h,
                 std::vector<uint8_t> &png) {
    if (!w || !h) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    png.assign(sig, sig + 8);

    std::vector<uint8_t> ihdr;
    putBE32(ihdr, w); putBE32(ihdr, h);
    ihdr.push_back(8);            // bit depth
    ihdr.push_back(6);            // color type 6 = RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    pngChunk(png, "IHDR", ihdr.data(), ihdr.size());

    // フィルタは全行 0 (None)。圧縮率より単純さを取る。
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
    for (uint32_t y = 0; y < h; ++y) {
      raw.push_back(0);
      raw.insert(raw.end(), rgba + size_t(y) * w * 4,
                 rgba + size_t(y + 1) * w * 4);
    }
    uLongf clen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> comp;
    comp.resize(size_t(clen));
    if (compress2(comp.data(), &clen, raw.data(), uLong(raw.size()), 9) != Z_OK)
      return false;
    pngChunk(png, "IDAT", comp.data(), size_t(clen));
    pngChunk(png, "IEND", nullptr, 0);
    return true;
  }

} // namespace clip
