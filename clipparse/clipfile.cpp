#include "clipfile.h"
#include "sqlite3.h"
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <functional>
#include <algorithm>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX          // windows.h の min/max マクロが std::min/max を壊す
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
    if (sqlite3_prepare_v2(db_, "SELECT CanvasWidth, CanvasHeight, CanvasRootFolder,"
                                " CanvasResolution FROM Canvas LIMIT 1",
                           -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        canvasW_ = sqlite3_column_int64(st, 0);
        canvasH_ = sqlite3_column_int64(st, 1);
        rootLayer_ = sqlite3_column_int64(st, 2);
        canvasRes_ = sqlite3_column_double(st, 3);
        if (canvasRes_ <= 0) canvasRes_ = 72.0;
      }
      sqlite3_finalize(st);
    }

    // 実ピクセル寸法はルートフォルダの 100% ミップから取る (下の topOffscreen が
    // layers_ を使わないので、この時点で引ける)。
    {
      sqlite3_stmt *m = nullptr;
      int64_t mip = 0, base = 0, offs = 0;
      if (sqlite3_prepare_v2(db_, "SELECT LayerRenderMipmap FROM Layer WHERE MainId=?",
                             -1, &m, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(m, 1, rootLayer_);
        if (sqlite3_step(m) == SQLITE_ROW) mip = sqlite3_column_int64(m, 0);
        sqlite3_finalize(m);
      }
      if (mip && sqlite3_prepare_v2(db_, "SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                                    -1, &m, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(m, 1, mip);
        if (sqlite3_step(m) == SQLITE_ROW) base = sqlite3_column_int64(m, 0);
        sqlite3_finalize(m);
      }
      if (base && sqlite3_prepare_v2(db_, "SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                                     -1, &m, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(m, 1, base);
        if (sqlite3_step(m) == SQLITE_ROW) offs = sqlite3_column_int64(m, 0);
        sqlite3_finalize(m);
      }
      if (offs) {
        if (const OffscreenAttr *a = attribute(offs)) {
          canvasPixelW_ = a->width;
          canvasPixelH_ = a->height;
        }
      }
      if (!canvasPixelW_) { canvasPixelW_ = canvasW_; canvasPixelH_ = canvasH_; }
    }

    // Layer をすべて読む (まず MainId をキーに素の行を集める)
    const char *sql =
      "SELECT MainId, LayerName, LayerType, LayerFolder, LayerVisibility,"
      " LayerOpacity, LayerComposite, LayerClip, LayerFirstChildIndex,"
      " LayerNextIndex, LayerRenderMipmap, LayerLayerMaskMipmap,"
      " LayerRenderThumbnail, LayerLayerMaskThumbnail FROM Layer";
    struct Row { LayerInfo li; int64_t firstChild = 0, next = 0; };
    std::map<int64_t, Row> rows;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      error_ = std::string("Layer query failed: ") + sqlite3_errmsg(db_);
      return false;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      Row r;
      LayerInfo &li = r.li;
      li.mainId     = sqlite3_column_int64(st, 0);
      if (const unsigned char *t = sqlite3_column_text(st, 1))
        li.name = (const char *)t;
      li.type       = sqlite3_column_int64(st, 2);
      li.folder     = sqlite3_column_int64(st, 3);
      li.visibility = sqlite3_column_int64(st, 4);
      li.opacity    = sqlite3_column_int64(st, 5);
      li.composite  = sqlite3_column_int64(st, 6);
      li.clipping   = sqlite3_column_int64(st, 7);
      r.firstChild  = sqlite3_column_int64(st, 8);
      r.next        = sqlite3_column_int64(st, 9);
      li.renderMipmap    = sqlite3_column_int64(st, 10);
      li.maskMipmap      = sqlite3_column_int64(st, 11);
      li.renderThumbnail = sqlite3_column_int64(st, 12);
      li.maskThumbnail   = sqlite3_column_int64(st, 13);
      li.isGroup  = (li.folder & 1) != 0;
      li.isFilter = (li.type & FILTER_BIT) != 0;
      li.hasMask  = (li.type & 2) != 0;
      rows[li.mainId] = r;
    }
    sqlite3_finalize(st);

    hasTextColumn_ = hasColumn("Layer", "TextLayerString");

    // ツリーを **psdparse と同じ平坦順** へ均す。
    // 中身を先に、フォルダ自身を後に積む (PSD はフォルダレイヤが中身より上)。
    // こうすると layers() の index 順がそのまま描画順になる。
    std::function<std::vector<int>(int64_t)> flatten = [&](int64_t parentId) {
      std::vector<int> here;
      auto pit = rows.find(parentId);
      if (pit == rows.end()) return here;
      int64_t child = pit->second.firstChild;
      while (child) {
        auto cit = rows.find(child);
        if (cit == rows.end()) break;
        std::vector<int> inner;
        if (cit->second.li.isGroup) inner = flatten(child);
        const int idx = int(layers_.size());
        layers_.push_back(cit->second.li);
        layerById_[child] = idx;
        for (int c : inner) layers_[(size_t)c].parent = idx;
        layers_[(size_t)idx].children = inner;
        here.push_back(idx);
        child = cit->second.next;
      }
      return here;
    };
    rootChildren_ = flatten(rootLayer_);
    for (int i : rootChildren_) layers_[(size_t)i].parent = -1;

    // 矩形とテキスト判定は Attribute / TLV を触るので、平坦化の後でまとめて。
    for (auto &li : layers_) {
      li.isText = hasTextColumn_ && layerHasText(li.mainId);
      li.bounds = computeBounds(li);
    }
    return true;
  }

  bool ClipFile::hasColumn(const char *table, const char *column) const {
    sqlite3_stmt *st = nullptr;
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
      return false;
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *n = sqlite3_column_text(st, 1);
      if (n && strcmp((const char *)n, column) == 0) { found = true; break; }
    }
    sqlite3_finalize(st);
    return found;
  }

  bool ClipFile::layerHasText(int64_t mainId) const {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT TextLayerString FROM Layer WHERE MainId=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, mainId);
    bool has = false;
    if (sqlite3_step(st) == SQLITE_ROW)
      has = sqlite3_column_bytes(st, 0) > 0;
    sqlite3_finalize(st);
    return has;
  }

  // テキストレイヤの配置位置を TextLayerAttributes の TLV タグ 42 から取る。
  // TLV の開始位置を求めるには手前のセクションを全部解く必要があるので、
  // 「タグ 42・長さ 16・幅が Offscreen と一致」で同定する (CLIP_FORMAT.md §2.3)。
  bool ClipFile::textOrigin(int64_t mainId, uint32_t w, uint32_t h,
                            int &x, int &y) const {
    (void)h;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT TextLayerAttributes FROM Layer WHERE MainId=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, mainId);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
      const uint8_t *b = (const uint8_t *)sqlite3_column_blob(st, 0);
      const int n = sqlite3_column_bytes(st, 0);
      for (int i = 0; b && i + 24 <= n; ++i) {
        if (leU32(b + i) != 42) continue;
        if (leU32(b + i + 4) != 16) continue;
        const int32_t x0 = (int32_t)leU32(b + i + 8);
        const int32_t y0 = (int32_t)leU32(b + i + 12);
        const int32_t x1 = (int32_t)leU32(b + i + 16);
        if (uint32_t(x1 - x0) == w - 1) { x = x0; y = y0; ok = true; break; }
      }
    }
    sqlite3_finalize(st);
    return ok;
  }

  // レイヤのラスタがキャンバス上で占める矩形。
  // 基本はキャンバス全面だが、テキスト等のオブジェクトレイヤは外接矩形サイズの
  // Offscreen を配置位置に置く。フォルダは psdparse に合わせて 0x0。
  Rect ClipFile::computeBounds(const LayerInfo &li) const {
    Rect r;
    if (li.isGroup) return r;
    const int64_t off = topOffscreen(li.mainId);
    if (!off) return r;
    const OffscreenAttr *a = attribute(off);
    if (!a) return r;
    if (!hasPixels(off) && !a->hasInitColor) {
      const int64_t obj = objectOffscreen(li.mainId);
      if (obj) {
        if (const OffscreenAttr *oa = attribute(obj)) {
          int x = 0, y = 0;
          if (textOrigin(li.mainId, oa->width, oa->height, x, y)) {
            r.x = x; r.y = y;
            r.w = int(oa->width); r.h = int(oa->height);
            return r;
          }
        }
      }
    }
    r.w = int(canvasPixelW_);
    r.h = int(canvasPixelH_);
    return r;
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

  // clipencode.h が同じものを使う (書く側の Attribute 解析)。
  // **InitColor の末尾が可変長**なのがここの肝。
  bool parseAttribute(const uint8_t *a, int len, OffscreenAttr &out) {
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

  bool ClipFile::readOffscreen(int64_t offscreenId, Image &out) const {
    uint32_t w = 0, h = 0;
    if (!readOffscreen(offscreenId, out.rgba, w, h)) return false;
    out.width = w; out.height = h;
    return true;
  }

  // 指定矩形に重なるブロックだけを展開する。CLIP は 256x256 タイルなので、
  // 大きなレイヤの一部を取るコストがタイル数に比例する (PSD では出来ない芸当)。
  bool ClipFile::readOffscreenRegion(int64_t offscreenId, const Rect &r,
                                     Image &out) const {
    const OffscreenAttr *a = attribute(offscreenId);
    if (!a || r.empty()) return false;
    out.resize(uint32_t(r.w), uint32_t(r.h));
    if (a->hasInitColor) {
      const uint32_t c = a->initColor;
      for (size_t i = 0; i < size_t(r.w) * r.h; i++) {
        out.rgba[i * 4 + 0] = uint8_t(c >> 24); out.rgba[i * 4 + 1] = uint8_t(c >> 16);
        out.rgba[i * 4 + 2] = uint8_t(c >> 8);  out.rgba[i * 4 + 3] = uint8_t(c);
      }
    }
    if (!hasPixels(offscreenId)) return true;

    const uint32_t bw = a->blockWidth, bh = a->blockHeight;
    if (!bw || !bh) return true;
    const int c0 = std::max(0, r.x / int(bw));
    const int c1 = std::min(int(a->cols) - 1, (r.x + r.w - 1) / int(bw));
    const int r0 = std::max(0, r.y / int(bh));
    const int r1 = std::min(int(a->rows) - 1, (r.y + r.h - 1) / int(bh));

    Block blk;
    for (int br = r0; br <= r1; br++) {
      for (int bc = c0; bc <= c1; bc++) {
        const uint32_t bi = uint32_t(br) * a->cols + uint32_t(bc);
        if (bi >= a->blockSizes.size()) continue;
        if (!readBlock(offscreenId, bi, blk) || blk.empty) continue;
        for (uint32_t y = 0; y < blk.height; y++) {
          const int cy = br * int(bh) + int(y);          // キャンバス座標
          const int dy = cy - r.y;
          if (dy < 0 || dy >= r.h) continue;
          for (uint32_t x = 0; x < blk.width; x++) {
            const int cx = bc * int(bw) + int(x);
            const int dx = cx - r.x;
            if (dx < 0 || dx >= r.w) continue;
            memcpy(out.at(uint32_t(dx), uint32_t(dy)),
                   &blk.rgba[(size_t(y) * blk.width + x) * 4], 4);
          }
        }
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
