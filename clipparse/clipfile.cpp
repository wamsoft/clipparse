#include "clipfile.h"
#include "sqlite3.h"
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace clip {

  // ---- OS マッピング (windows.h を漏らさないための pimpl) -----------------

  struct ClipFile::Mapping {
    const uint8_t *base = nullptr;
    int64_t size = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
#else
    int fd = -1;
#endif
    ~Mapping() { close(); }

    bool open(const char *utf8Path) {
#ifdef _WIN32
      int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, nullptr, 0);
      std::wstring w(size_t(wlen), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, &w[0], wlen);
      file = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) return false;
      LARGE_INTEGER li;
      if (!GetFileSizeEx(file, &li)) return false;
      size = li.QuadPart;
      mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
      if (!mapping) return false;
      base = (const uint8_t *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
      return base != nullptr;
#else
      fd = ::open(utf8Path, O_RDONLY);
      if (fd < 0) return false;
      struct stat st;
      if (fstat(fd, &st) != 0) return false;
      size = st.st_size;
      void *p = mmap(nullptr, size_t(size), PROT_READ, MAP_PRIVATE, fd, 0);
      if (p == MAP_FAILED) return false;
      base = (const uint8_t *)p;
      return true;
#endif
    }

    void close() {
#ifdef _WIN32
      if (base) UnmapViewOfFile(base);
      if (mapping) CloseHandle(mapping);
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
      base = nullptr; mapping = nullptr; file = INVALID_HANDLE_VALUE;
#else
      if (base) munmap((void *)base, size_t(size));
      if (fd >= 0) ::close(fd);
      base = nullptr; fd = -1;
#endif
    }
  };

  ClipFile::ClipFile() {}
  ClipFile::~ClipFile() { clear(); }

  void ClipFile::clear() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    source_.reset();
    mapping_.reset();
    owned_.clear();
    extOffset_.clear();
    layers_.clear();
    layerById_.clear();
    attrCache_.clear();
    canvasW_ = canvasH_ = rootLayer_ = 0;
  }

  bool ClipFile::load(const char *filename) {
    clear();
    mapping_.reset(new Mapping);
    if (!mapping_->open(filename)) {
      error_ = "cannot open/map file";
      return false;
    }
    source_.reset(new MemorySource(mapping_->base, mapping_->size));
    return parse();
  }

  bool ClipFile::loadFromMemory(const uint8_t *data, int64_t size) {
    clear();
    source_.reset(new MemorySource(data, size));
    return parse();
  }

  // ---- チャンク走査 + SQLite ---------------------------------------------

  bool ClipFile::parse() {
    const uint8_t *p = source_->data();
    const int64_t n = source_->size();
    if (n < 64 || memcmp(p, "CSFCHUNK", 8) != 0) {
      error_ = "not a .clip file";
      return false;
    }
    const int64_t headerLen = int64_t(beU64(p + 16));

    // CHNKHead.binary_section_size が CHNKSQLi チャンクヘッダの位置そのもの。
    // ここを信じれば SQLite へ直行できるが、外部チャンクの索引も要るので
    // 結局チャンクは歩く (ヘッダだけ読み、ペイロードは飛ばす)。
    int64_t pos = headerLen;
    while (pos + 16 <= n) {
      const uint8_t *h = p + pos;
      const int64_t clen = int64_t(beU64(h + 8));
      const int64_t body = pos + 16;
      if (memcmp(h, "CHNKHead", 8) == 0) {
        pos = body + clen;
      } else if (memcmp(h, "CHNKExta", 8) == 0) {
        const uint8_t *e = p + body;
        const int64_t dsize = int64_t(beU64(e + 48));
        extOffset_[std::string((const char *)e + 8, 40)] = pos;
        pos = body + 56 + dsize;
      } else if (memcmp(h, "CHNKSQLi", 8) == 0) {
        dbOffset_ = body;
        dbSize_ = clen;
        pos = body + clen;
      } else if (memcmp(h, "CHNKFoot", 8) == 0) {
        break;
      } else {
        error_ = "unknown chunk";
        return false;
      }
    }
    if (!dbSize_) {
      error_ = "no CHNKSQLi chunk";
      return false;
    }
    return openDb();
  }

  bool ClipFile::openDb() {
    if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
      error_ = "sqlite3_open failed";
      return false;
    }
    // mmap 上の SQLite をゼロコピーで読む。FREEONCLOSE も RESIZEABLE も
    // 付けないので、SQLite は渡したバッファを読むだけで解放しない。
    const uint8_t *dbp = source_->data() + dbOffset_;
    int rc = sqlite3_deserialize(db_, "main", const_cast<uint8_t *>(dbp),
                                 dbSize_, dbSize_,
                                 SQLITE_DESERIALIZE_READONLY);
    if (rc != SQLITE_OK) {
      error_ = std::string("sqlite3_deserialize failed: ") + sqlite3_errmsg(db_);
      return false;
    }

    sqlite3_stmt *st = nullptr;
    // Canvas
    if (sqlite3_prepare_v2(db_, "SELECT CanvasWidth, CanvasHeight, CanvasRootFolder"
                                " FROM Canvas LIMIT 1", -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        canvasW_ = sqlite3_column_int64(st, 0);
        canvasH_ = sqlite3_column_int64(st, 1);
        rootLayer_ = sqlite3_column_int64(st, 2);
      }
      sqlite3_finalize(st);
    }

    // Layer をすべて読む
    const char *sql =
      "SELECT MainId, LayerName, LayerType, LayerFolder, LayerVisibility,"
      " LayerOpacity, LayerComposite, LayerClip, LayerFirstChildIndex,"
      " LayerNextIndex, LayerRenderMipmap, LayerLayerMaskMipmap,"
      " LayerRenderThumbnail, LayerLayerMaskThumbnail FROM Layer";
    std::map<int64_t, std::pair<int64_t, int64_t>> links;   // id -> (firstChild, next)
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      error_ = std::string("Layer query failed: ") + sqlite3_errmsg(db_);
      return false;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      LayerInfo li;
      li.mainId     = sqlite3_column_int64(st, 0);
      if (const unsigned char *t = sqlite3_column_text(st, 1))
        li.name = (const char *)t;
      li.type       = sqlite3_column_int64(st, 2);
      li.folder     = sqlite3_column_int64(st, 3);
      li.visibility = sqlite3_column_int64(st, 4);
      li.opacity    = sqlite3_column_int64(st, 5);
      li.composite  = sqlite3_column_int64(st, 6);
      li.clipping   = sqlite3_column_int64(st, 7);
      links[li.mainId] = { sqlite3_column_int64(st, 8), sqlite3_column_int64(st, 9) };
      li.renderMipmap    = sqlite3_column_int64(st, 10);
      li.maskMipmap      = sqlite3_column_int64(st, 11);
      li.renderThumbnail = sqlite3_column_int64(st, 12);
      li.maskThumbnail   = sqlite3_column_int64(st, 13);
      layerById_[li.mainId] = int(layers_.size());
      layers_.push_back(li);
    }
    sqlite3_finalize(st);

    // 親子リンクを解決する (子チェーンの先頭が最下層)
    for (auto &kv : links) {
      auto it = layerById_.find(kv.first);
      if (it == layerById_.end()) continue;
      int64_t child = kv.second.first;
      while (child) {
        auto ci = layerById_.find(child);
        if (ci == layerById_.end()) break;
        layers_[ci->second].parent = it->second;
        layers_[it->second].children.push_back(ci->second);
        child = links[child].second;
      }
    }
    return true;
  }

  int ClipFile::layerIndex(int64_t mainId) const {
    auto it = layerById_.find(mainId);
    return it == layerById_.end() ? -1 : it->second;
  }

  // ---- ミップ連鎖 ---------------------------------------------------------

  int64_t ClipFile::topOffscreen(int64_t layerMainId, bool mask) const {
    int idx = layerIndex(layerMainId);
    if (idx < 0) return 0;
    const int64_t mipmapId = mask ? layers_[idx].maskMipmap : layers_[idx].renderMipmap;
    if (!mipmapId) return 0;

    // Mipmap.BaseMipmapInfo → MipmapInfo.Offscreen。
    // MipmapInfo を (LayerId, ThisScale=100) で引いてはいけない:
    // マスクを持つレイヤは描画用とマスク用の 2 本の連鎖を持ち、
    // どちらも同じ LayerId・同じ ThisScale=100.0 で始まる。
    sqlite3_stmt *st = nullptr;
    int64_t base = 0;
    if (sqlite3_prepare_v2(db_, "SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, mipmapId);
      if (sqlite3_step(st) == SQLITE_ROW) base = sqlite3_column_int64(st, 0);
      sqlite3_finalize(st);
    }
    if (!base) return 0;
    int64_t offs = 0;
    if (sqlite3_prepare_v2(db_, "SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, base);
      if (sqlite3_step(st) == SQLITE_ROW) offs = sqlite3_column_int64(st, 0);
      sqlite3_finalize(st);
    }
    return offs;
  }

  int64_t ClipFile::objectOffscreen(int64_t layerMainId) const {
    // ミップ段でもサムネイルでもない Offscreen。どの FK からも参照されないので
    // Offscreen.LayerId で引くしかない。
    const char *sql =
      "SELECT MainId FROM Offscreen WHERE LayerId=?"
      " AND MainId NOT IN (SELECT Offscreen FROM MipmapInfo)"
      " AND MainId NOT IN (SELECT ThumbnailOffscreen FROM LayerThumbnail"
      "                    WHERE ThumbnailOffscreen IS NOT NULL)";
    sqlite3_stmt *st = nullptr;
    int64_t found = 0;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, layerMainId);
      while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t m = sqlite3_column_int64(st, 0);
        if (hasPixels(m)) { found = m; break; }
      }
      sqlite3_finalize(st);
    }
    return found;
  }

  // ---- Offscreen.Attribute ------------------------------------------------

  static bool parseAttribute(const uint8_t *a, int len, OffscreenAttr &out) {
    if (len < 16) return false;
    int p = 16;                                   // section_sizes[4] を飛ばす
    const char *names[3] = { "Parameter", "InitColor", "BlockSize" };
    for (int sec = 0; sec < 3; ++sec) {
      if (p + 4 + 18 > len) return false;
      p += 4;                                     // 境界マーカー (=9)
      if (utf16beToAscii(a + p, 9) != names[sec]) return false;
      p += 18;
      if (sec == 0) {
        if (p + 80 > len) return false;
        out.width  = beU32(a + p +  0); out.height = beU32(a + p +  4);
        out.cols   = beU32(a + p +  8); out.rows   = beU32(a + p + 12);
        out.colorMode   = beU32(a + p + 16);
        out.numChannels = beU32(a + p + 24);
        out.bitDepth    = beU32(a + p + 28);
        out.planeBytes  = beU32(a + p + 32);
        out.planeCount  = beU32(a + p + 36);
        out.rowBytes    = beU32(a + p + 40);
        out.blockHeight = beU32(a + p + 56);
        out.blockWidth  = beU32(a + p + 60);
        p += 80;
      } else if (sec == 1) {
        if (p + 20 > len) return false;
        p += 4;                                   // magic (=20)
        const uint32_t hasColor = beU32(a + p);
        out.initColor    = beU32(a + p + 4);
        const uint32_t numExtra = beU32(a + p + 8);
        out.hasInitColor = hasColor != 0;
        p += 16;
        // 末尾は可変長。has_color だけを見て 16 バイト固定で読むと
        // num_channels==0 の面 (マスク等) で壊れる。
        p += int(4 * numExtra);
      } else {
        if (p + 12 > len) return false;
        const uint32_t nblocks = beU32(a + p + 4);
        p += 12;
        if (p + int(4 * nblocks) > len) return false;
        out.blockSizes.resize(nblocks);
        out.blockOffsets.resize(nblocks);
        uint64_t acc = 0;
        for (uint32_t i = 0; i < nblocks; ++i) {
          out.blockSizes[i] = beU32(a + p + 4 * i);
          out.blockOffsets[i] = acc;              // 前置和 → O(1) で位置が出る
          acc += out.blockSizes[i];
        }
        p += int(4 * nblocks);
      }
    }
    return true;
  }

  const OffscreenAttr *ClipFile::attribute(int64_t offscreenId) const {
    auto it = attrCache_.find(offscreenId);
    if (it != attrCache_.end()) return &it->second;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT Attribute, BlockData FROM Offscreen WHERE MainId=?",
                           -1, &st, nullptr) != SQLITE_OK) return nullptr;
    sqlite3_bind_int64(st, 1, offscreenId);
    const OffscreenAttr *result = nullptr;
    if (sqlite3_step(st) == SQLITE_ROW) {
      const uint8_t *blob = (const uint8_t *)sqlite3_column_blob(st, 0);
      const int blen = sqlite3_column_bytes(st, 0);
      OffscreenAttr attr;
      if (blob && parseAttribute(blob, blen, attr)) {
        if (const unsigned char *t = sqlite3_column_text(st, 1))
          attr.blockDataId = (const char *)t;
        result = &(attrCache_[offscreenId] = attr);
      }
    }
    sqlite3_finalize(st);
    return result;
  }

  bool ClipFile::hasPixels(int64_t offscreenId) const {
    const OffscreenAttr *a = attribute(offscreenId);
    // BlockSize[] に値が入っていても実体があるとは限らない。
    // 実体の有無は ExternalChunk に載っているかだけで決まる。
    return a && extOffset_.count(a->blockDataId) != 0;
  }

  // ---- ブロック展開 -------------------------------------------------------

  static void decodeRGBA(const uint8_t *buf, uint32_t bw, uint32_t bh,
                         std::vector<uint8_t> &out) {
    // rows[64:] が B,G,R,(未使用)、rows[0:64] が 4x4 スーパーピクセルに
    // 畳まれたアルファ面を幅 64 の 4 タイルに分割したもの。
    const uint32_t TILE = 64;
    out.resize(size_t(bw) * bh * 4);
    const uint8_t *color = buf + size_t(TILE) * bw * 4;
    for (uint32_t y = 0; y < bh; ++y) {
      for (uint32_t x = 0; x < bw; ++x) {
        const uint8_t *s = color + (size_t(y) * bw + x) * 4;
        uint8_t *d = out.data() + (size_t(y) * bw + x) * 4;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0];        // BGR -> RGB
        // アルファ面の逆写像。alpha[y][x] は 4x4 スーパーピクセルに畳まれており
        //   r = y/4, i = y%4, c = x/4, j = x%4
        //   alpha = tile[r][64*i + c][j]
        // (タイル番号 i は **y** から、チャンネル j は **x** から取る)
        const uint32_t r = y / 4, i = y % 4;
        const uint32_t c = x / 4, j = x % 4;
        d[3] = buf[(size_t(r) * bw + TILE * i + c) * 4 + j];
      }
    }
  }

  bool ClipFile::readBlock(int64_t offscreenId, uint32_t blockIndex, Block &out) const {
    const OffscreenAttr *a = attribute(offscreenId);
    if (!a || blockIndex >= a->blockSizes.size()) return false;
    auto it = extOffset_.find(a->blockDataId);
    if (it == extOffset_.end()) return false;

    // SQLite だけで決まる絶対位置。バイナリ領域の走査は不要。
    const int64_t dataStart = it->second + 16 + 56;
    const int64_t p = dataStart + int64_t(a->blockOffsets[blockIndex]);
    const uint8_t *base = source_->data();
    if (p + 74 > source_->size()) return false;

    const uint32_t recSize = beU32(base + p);
    const uint32_t nameLen = beU32(base + p + 4);
    if (recSize != a->blockSizes[blockIndex]) return false;
    if (utf16beToAscii(base + p + 8, 19) != "BlockDataBeginChunk") return false;
    const uint8_t *body = base + p + 8 + nameLen * 2;
    const uint32_t idx     = beU32(body);
    const uint32_t declen  = beU32(body + 4);
    out.width  = beU32(body + 8);
    out.height = beU32(body + 12);
    const uint32_t has = beU32(body + 16);
    if (idx != blockIndex) return false;
    if (!has) { out.empty = true; out.rgba.clear(); return true; }

    const uint32_t section = beU32(body + 20);          // BE
    const uint32_t clen    = leU32(body + 24);          // ★ここだけ LE
    if (section != clen + 4) return false;

    std::vector<uint8_t> raw(declen);
    uLongf dstLen = declen;
    if (uncompress(raw.data(), &dstLen, body + 28, clen) != Z_OK || dstLen != declen)
      return false;

    out.empty = false;
    const uint32_t bw = out.width, bh = out.height;
    if (a->numChannels == 4) {
      decodeRGBA(raw.data(), bw, bh, out.rgba);
    } else if (a->numChannels == 0) {
      // マスク / 選択範囲: アルファ 1 面のみ
      out.rgba.assign(size_t(bw) * bh * 4, 255);
      for (size_t i = 0; i < size_t(bw) * bh; ++i) out.rgba[i * 4 + 3] = raw[i];
    } else {
      // グレー / モノクロ: plane0 = α, plane1 = 値
      out.rgba.resize(size_t(bw) * bh * 4);
      const bool oneBit = (a->bitDepth == 1);
      for (size_t i = 0; i < size_t(bw) * bh; ++i) {
        uint8_t alpha, value;
        if (oneBit) {
          const size_t byteIdx = i / 8, bit = 7 - (i % 8);
          alpha = (raw[byteIdx] >> bit & 1) ? 255 : 0;
          value = (raw[a->planeBytes + byteIdx] >> bit & 1) ? 255 : 0;
        } else {
          alpha = raw[i];
          value = raw[size_t(a->planeBytes) + i];
        }
        out.rgba[i * 4 + 0] = out.rgba[i * 4 + 1] = out.rgba[i * 4 + 2] = value;
        out.rgba[i * 4 + 3] = alpha;
      }
    }
    return true;
  }

  bool ClipFile::readOffscreen(int64_t offscreenId, std::vector<uint8_t> &rgba,
                               uint32_t &w, uint32_t &h) const {
    const OffscreenAttr *a = attribute(offscreenId);
    if (!a) return false;
    w = a->width; h = a->height;
    rgba.assign(size_t(w) * h * 4, 0);
    if (a->hasInitColor) {
      const uint32_t c = a->initColor;
      for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgba[i * 4 + 0] = uint8_t(c >> 24); rgba[i * 4 + 1] = uint8_t(c >> 16);
        rgba[i * 4 + 2] = uint8_t(c >> 8);  rgba[i * 4 + 3] = uint8_t(c);
      }
    }
    if (!hasPixels(offscreenId)) return true;

    Block blk;
    for (uint32_t i = 0; i < a->blockSizes.size(); ++i) {
      if (!readBlock(offscreenId, i, blk) || blk.empty) continue;
      const uint32_t br = i / a->cols, bc = i % a->cols;
      for (uint32_t y = 0; y < blk.height; ++y) {
        const uint32_t dy = br * blk.height + y;
        if (dy >= h) break;
        const uint32_t copyW = (bc * blk.width + blk.width <= w)
                             ? blk.width : (w > bc * blk.width ? w - bc * blk.width : 0);
        if (!copyW) continue;
        memcpy(&rgba[(size_t(dy) * w + bc * blk.width) * 4],
               &blk.rgba[size_t(y) * blk.width * 4], size_t(copyW) * 4);
      }
    }
    return true;
  }

  // ---- 診断 ---------------------------------------------------------------

  bool ClipFile::checkAll(std::string *report) const {
    std::ostringstream os;
    int nOff = 0, nWith = 0, nBlocks = 0, nData = 0, nBad = 0;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT MainId FROM Offscreen", -1, &st, nullptr) != SQLITE_OK)
      return false;
    std::vector<int64_t> ids;
    while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);

    Block blk;
    for (int64_t id : ids) {
      ++nOff;
      const OffscreenAttr *a = attribute(id);
      if (!a) { ++nBad; os << "  Attribute のパース失敗: offscreen " << id << "\n"; continue; }
      if (!hasPixels(id)) continue;
      ++nWith;
      for (uint32_t i = 0; i < a->blockSizes.size(); ++i) {
        ++nBlocks;
        if (!readBlock(id, i, blk)) {
          ++nBad;
          os << "  ブロック展開失敗: offscreen " << id << " block " << i << "\n";
          continue;
        }
        if (!blk.empty) ++nData;
        else if (a->blockSizes[i] != 104) {
          ++nBad;
          os << "  空ブロックのサイズが 104 でない: offscreen " << id
             << " block " << i << " size " << a->blockSizes[i] << "\n";
        }
      }
    }
    os << "  Offscreen " << nOff << " 行 / 実体あり " << nWith
       << " / ブロック " << nBlocks << " (画素あり " << nData << ")"
       << " / 異常 " << nBad << "\n";
    if (report) *report = os.str();
    return nBad == 0;
  }

} // namespace clip
