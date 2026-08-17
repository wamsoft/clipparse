#include "clipwrite.h"
#include "clipencode.h"
#include "sqlite3.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>

namespace clip {

  namespace {

    bool readWholeFile(const char *path, std::vector<uint8_t> &out) {
      FILE *fp = fopen(path, "rb");
      if (!fp) return false;
      fseek(fp, 0, SEEK_END);
      const long n = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      out.resize(size_t(n));
      const bool ok = n == 0 || fread(out.data(), 1, size_t(n), fp) == size_t(n);
      fclose(fp);
      return ok;
    }

    void putBE32(std::vector<uint8_t> &v, uint32_t x) {
      v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
      v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
    }
    void putBE64(std::vector<uint8_t> &v, uint64_t x) {
      putBE32(v, uint32_t(x >> 32)); putBE32(v, uint32_t(x));
    }
    void putBE64At(std::vector<uint8_t> &v, size_t at, uint64_t x) {
      for (int i = 0; i < 8; ++i) v[at + size_t(i)] = uint8_t(x >> (56 - i * 8));
    }

    // 単発の SQL。失敗したら false。
    bool exec(sqlite3 *db, const char *sql) {
      char *msg = nullptr;
      const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
      if (msg) sqlite3_free(msg);
      return rc == SQLITE_OK;
    }

    // 1 値だけ取る系のヘルパ
    int64_t queryInt(sqlite3 *db, const char *sql, int64_t arg = -1,
                     bool *found = nullptr) {
      sqlite3_stmt *st = nullptr;
      int64_t v = 0;
      if (found) *found = false;
      if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
      if (arg >= 0) sqlite3_bind_int64(st, 1, arg);
      if (sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
        if (found) *found = true;
      }
      sqlite3_finalize(st);
      return v;
    }

    bool queryBlob(sqlite3 *db, const char *sql, int64_t arg,
                   std::vector<uint8_t> &out) {
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
      sqlite3_bind_int64(st, 1, arg);
      bool ok = false;
      if (sqlite3_step(st) == SQLITE_ROW) {
        const uint8_t *p = (const uint8_t *)sqlite3_column_blob(st, 0);
        const int n = sqlite3_column_bytes(st, 0);
        if (p) { out.assign(p, p + n); ok = true; }
      }
      sqlite3_finalize(st);
      return ok;
    }

    std::string queryText(sqlite3 *db, const char *sql, int64_t arg) {
      sqlite3_stmt *st = nullptr;
      std::string s;
      if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return s;
      sqlite3_bind_int64(st, 1, arg);
      if (sqlite3_step(st) == SQLITE_ROW) {
        const uint8_t *p = (const uint8_t *)sqlite3_column_blob(st, 0);
        const int n = sqlite3_column_bytes(st, 0);
        if (p) s.assign((const char *)p, size_t(n));
      }
      sqlite3_finalize(st);
      return s;
    }

    std::vector<std::string> tableColumns(sqlite3 *db, const char *table) {
      std::vector<std::string> cols;
      std::string sql = std::string("PRAGMA table_info([") + table + "])";
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return cols;
      while (sqlite3_step(st) == SQLITE_ROW)
        cols.push_back((const char *)sqlite3_column_text(st, 1));
      sqlite3_finalize(st);
      return cols;
    }

  } // namespace

  // ---- 生存期間 -----------------------------------------------------------

  ClipWriter::ClipWriter() {}
  ClipWriter::~ClipWriter() { clear(); }

  void ClipWriter::clear() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    headBody_.clear();
    externals_.clear();
    headerLen_ = footLen_ = 0;
    error_.clear();
  }

  void ClipWriter::setExternalIdSeed(uint64_t seed) {
    idState_ = seed;
    idSeeded_ = true;
  }

  std::string ClipWriter::newExternalId() {
    // `extrnlid` + 32 桁の大文字 16 進 = 40 バイト。CSP と同じ形。
    if (!idSeeded_) {
      std::random_device rd;
      idState_ = (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ 0x9E3779B97F4A7C15ull;
      idSeeded_ = true;
    }
    static const char *HEX = "0123456789ABCDEF";
    std::string s = "extrnlid";
    for (int i = 0; i < 32; ++i) {
      idState_ ^= idState_ << 13;
      idState_ ^= idState_ >> 7;
      idState_ ^= idState_ << 17;
      s.push_back(HEX[(idState_ >> 28) & 0xF]);
    }
    return s;
  }

  // ---- 読み込み -----------------------------------------------------------

  bool ClipWriter::load(const char *path) {
    clear();
    std::vector<uint8_t> raw;
    if (!readWholeFile(path, raw)) {
      error_ = "cannot read file";
      return false;
    }
    if (raw.size() < 64 || memcmp(raw.data(), "CSFCHUNK", 8) != 0) {
      error_ = "not a .clip file";
      return false;
    }
    const uint64_t fileSize = beU64(raw.data() + 8);
    headerLen_ = int64_t(beU64(raw.data() + 16));
    if (fileSize != raw.size())
      error_ = "warning: header size mismatch";

    std::vector<uint8_t> dbImage;
    int64_t pos = headerLen_;
    const int64_t n = int64_t(raw.size());
    while (pos + 16 <= n) {
      const uint8_t *h = raw.data() + pos;
      const int64_t clen = int64_t(beU64(h + 8));
      const int64_t body = pos + 16;
      if (memcmp(h, "CHNKHead", 8) == 0) {
        headBody_.assign(raw.begin() + body, raw.begin() + body + clen);
        pos = body + clen;
      } else if (memcmp(h, "CHNKExta", 8) == 0) {
        const uint8_t *e = raw.data() + body;
        const int64_t dsize = int64_t(beU64(e + 48));
        externals_.emplace_back(std::string((const char *)e + 8, 40),
                                std::vector<uint8_t>(raw.begin() + body + 56,
                                                     raw.begin() + body + 56 + dsize));
        pos = body + 56 + dsize;
      } else if (memcmp(h, "CHNKSQLi", 8) == 0) {
        dbImage.assign(raw.begin() + body, raw.begin() + body + clen);
        pos = body + clen;
      } else if (memcmp(h, "CHNKFoot", 8) == 0) {
        footLen_ = clen;
        break;
      } else {
        error_ = "unknown chunk";
        return false;
      }
    }
    if (dbImage.empty()) {
      error_ = "no CHNKSQLi chunk";
      return false;
    }

    if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
      error_ = "sqlite3_open failed";
      return false;
    }
    // **書ける**メモリ DB にする。SQLite に所有させるので sqlite3_malloc64 で確保。
    uint8_t *buf = (uint8_t *)sqlite3_malloc64(sqlite3_uint64(dbImage.size()));
    if (!buf) {
      error_ = "out of memory";
      return false;
    }
    memcpy(buf, dbImage.data(), dbImage.size());
    if (sqlite3_deserialize(db_, "main", buf, sqlite3_int64(dbImage.size()),
                            sqlite3_int64(dbImage.size()),
                            SQLITE_DESERIALIZE_RESIZEABLE |
                            SQLITE_DESERIALIZE_FREEONCLOSE) != SQLITE_OK) {
      error_ = std::string("sqlite3_deserialize failed: ") + sqlite3_errmsg(db_);
      return false;
    }
    return true;
  }

  // ---- 書き出し -----------------------------------------------------------

  int64_t ClipWriter::save(const char *path) {
    if (!db_) { error_ = "not loaded"; return 0; }

    // 1) 新しいオフセットを先に全部計算する (CHNKExta ヘッダ先頭の絶対位置)
    int64_t pos = headerLen_ + 16 + int64_t(headBody_.size());
    std::vector<int64_t> offsets;
    offsets.reserve(externals_.size());
    for (const auto &e : externals_) {
      offsets.push_back(pos);
      pos += 16 + 56 + int64_t(e.second.size());
    }
    const int64_t binarySectionSize = pos;    // = CHNKSQLi チャンクヘッダの位置

    // 2) ExternalChunk を更新する。
    //    **`ExternalID` は BLOB 宣言だが値は TEXT**。bytes を束縛すると
    //    1 行もマッチせず黙って失敗する (`Offscreen.BlockData` は逆に BLOB)。
    std::set<std::string> have;
    {
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, "SELECT ExternalID FROM ExternalChunk",
                             -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
          const uint8_t *p = (const uint8_t *)sqlite3_column_blob(st, 0);
          have.insert(std::string((const char *)p, size_t(sqlite3_column_bytes(st, 0))));
        }
        sqlite3_finalize(st);
      }
    }
    exec(db_, "BEGIN");
    std::set<std::string> live;
    for (size_t i = 0; i < externals_.size(); ++i) {
      const std::string &id = externals_[i].first;
      live.insert(id);
      sqlite3_stmt *st = nullptr;
      const char *sql = have.count(id)
          ? "UPDATE ExternalChunk SET Offset=? WHERE ExternalID=?"
          : "INSERT INTO ExternalChunk (Offset, ExternalID) VALUES (?, ?)";
      if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        error_ = sqlite3_errmsg(db_);
        exec(db_, "ROLLBACK");
        return 0;
      }
      sqlite3_bind_int64(st, 1, offsets[i]);
      sqlite3_bind_text(st, 2, id.c_str(), int(id.size()), SQLITE_TRANSIENT);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    for (const std::string &id : have) {
      if (live.count(id)) continue;
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, "DELETE FROM ExternalChunk WHERE ExternalID=?",
                             -1, &st, nullptr) != SQLITE_OK) continue;
      sqlite3_bind_text(st, 1, id.c_str(), int(id.size()), SQLITE_TRANSIENT);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    exec(db_, "COMMIT");

    const int64_t rows = queryInt(db_, "SELECT COUNT(*) FROM ExternalChunk");
    if (rows != int64_t(externals_.size())) {
      char buf[128];
      snprintf(buf, sizeof buf, "ExternalChunk rows %lld != chunks %zu",
               (long long)rows, externals_.size());
      error_ = buf;
      return 0;
    }

    // 3) SQLite を直列化する
    sqlite3_int64 dbSize = 0;
    unsigned char *dbBytes = sqlite3_serialize(db_, "main", &dbSize, 0);
    if (!dbBytes) {
      error_ = "sqlite3_serialize failed";
      return 0;
    }

    // 4) CHNKHead の binary_section_size を差し替える
    std::vector<uint8_t> head = headBody_;
    if (head.size() >= 16) putBE64At(head, 8, uint64_t(binarySectionSize));

    std::vector<uint8_t> out;
    out.reserve(size_t(binarySectionSize + dbSize + 64));
    out.insert(out.end(), (const uint8_t *)"CSFCHUNK", (const uint8_t *)"CSFCHUNK" + 8);
    putBE64(out, 0);                       // サイズは後で埋める
    putBE64(out, uint64_t(headerLen_));
    out.insert(out.end(), (const uint8_t *)"CHNKHead", (const uint8_t *)"CHNKHead" + 8);
    putBE64(out, uint64_t(head.size()));
    out.insert(out.end(), head.begin(), head.end());
    for (const auto &e : externals_) {
      out.insert(out.end(), (const uint8_t *)"CHNKExta", (const uint8_t *)"CHNKExta" + 8);
      putBE64(out, uint64_t(56 + e.second.size()));
      putBE64(out, 40);
      out.insert(out.end(), e.first.begin(), e.first.end());
      putBE64(out, uint64_t(e.second.size()));
      out.insert(out.end(), e.second.begin(), e.second.end());
    }
    if (int64_t(out.size()) != binarySectionSize) {
      error_ = "internal: binary section size mismatch";
      sqlite3_free(dbBytes);
      return 0;
    }
    out.insert(out.end(), (const uint8_t *)"CHNKSQLi", (const uint8_t *)"CHNKSQLi" + 8);
    putBE64(out, uint64_t(dbSize));
    out.insert(out.end(), dbBytes, dbBytes + dbSize);
    sqlite3_free(dbBytes);
    out.insert(out.end(), (const uint8_t *)"CHNKFoot", (const uint8_t *)"CHNKFoot" + 8);
    putBE64(out, uint64_t(footLen_));
    putBE64At(out, 8, uint64_t(out.size()));

    FILE *fp = fopen(path, "wb");
    if (!fp) { error_ = "cannot write file"; return 0; }
    const size_t wrote = fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    if (wrote != out.size()) { error_ = "short write"; return 0; }
    return int64_t(out.size());
  }

  // ---- 外部チャンクの差し替え ---------------------------------------------

  void ClipWriter::replaceExternal(const std::string &id,
                                   std::vector<uint8_t> &&payload) {
    for (auto &e : externals_)
      if (e.first == id) { e.second = std::move(payload); return; }
    externals_.emplace_back(id, std::move(payload));
  }

  void ClipWriter::removeExternal(const std::string &id) {
    externals_.erase(std::remove_if(externals_.begin(), externals_.end(),
                                    [&](const std::pair<std::string,
                                                        std::vector<uint8_t>> &e) {
                                      return e.first == id;
                                    }),
                     externals_.end());
  }

  // ---- 行の複製 -----------------------------------------------------------

  int64_t ClipWriter::nextId(const char *table) {
    sqlite3_stmt *st = nullptr;
    int64_t cur = 0;
    bool found = false;
    if (sqlite3_prepare_v2(db_, "SELECT MaxIndex FROM ElemScheme WHERE TableName=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) { cur = sqlite3_column_int64(st, 0); found = true; }
      sqlite3_finalize(st);
    }
    if (!found) {
      std::string sql = std::string("SELECT COALESCE(MAX(MainId),0) FROM [") + table + "]";
      cur = queryInt(db_, sql.c_str());
    }
    const int64_t id = cur + 1;
    if (found &&
        sqlite3_prepare_v2(db_, "UPDATE ElemScheme SET MaxIndex=? WHERE TableName=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, id);
      sqlite3_bind_text(st, 2, table, -1, SQLITE_TRANSIENT);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    return id;
  }

  bool ClipWriter::copyRow(const char *table, const char *whereCol, int64_t whereVal,
                           const std::vector<std::pair<std::string, std::string>> &blobOver,
                           const std::vector<std::pair<std::string, std::string>> &txtOver,
                           const std::vector<std::pair<std::string, int64_t>> &i64Over,
                           const std::vector<std::string> &nullOver) {
    // **列を列挙せず丸ごと写す**。Layer 57 列 / LayerThumbnail 43 列の既定値は
    // ほとんど意味が分かっていないので、雛形行から引き継ぐのが安全。
    // 値は sqlite3_bind_value で写すので、**格納型 (BLOB/TEXT) も保たれる**。
    std::vector<std::string> cols = tableColumns(db_, table);
    cols.erase(std::remove(cols.begin(), cols.end(), std::string("_PW_ID")),
               cols.end());
    if (cols.empty()) { error_ = "no such table"; return false; }

    std::string sel = "SELECT ";
    for (size_t i = 0; i < cols.size(); ++i)
      sel += (i ? ",[" : "[") + cols[i] + "]";
    sel += std::string(" FROM [") + table + "] WHERE [" + whereCol + "]=?";

    sqlite3_stmt *src = nullptr;
    if (sqlite3_prepare_v2(db_, sel.c_str(), -1, &src, nullptr) != SQLITE_OK) {
      error_ = sqlite3_errmsg(db_);
      return false;
    }
    sqlite3_bind_int64(src, 1, whereVal);
    if (sqlite3_step(src) != SQLITE_ROW) {
      sqlite3_finalize(src);
      error_ = std::string(table) + ": template row not found";
      return false;
    }

    std::string ins = std::string("INSERT INTO [") + table + "] (";
    for (size_t i = 0; i < cols.size(); ++i) ins += (i ? ",[" : "[") + cols[i] + "]";
    ins += ") VALUES (";
    for (size_t i = 0; i < cols.size(); ++i) ins += i ? ",?" : "?";
    ins += ")";

    sqlite3_stmt *dst = nullptr;
    if (sqlite3_prepare_v2(db_, ins.c_str(), -1, &dst, nullptr) != SQLITE_OK) {
      sqlite3_finalize(src);
      error_ = sqlite3_errmsg(db_);
      return false;
    }
    for (size_t i = 0; i < cols.size(); ++i) {
      const int at = int(i) + 1;
      bool done = false;
      for (const auto &o : i64Over)
        if (o.first == cols[i]) { sqlite3_bind_int64(dst, at, o.second); done = true; }
      // **BLOB として束縛する列**。ここを TEXT にすると CSP は実体を
      // 解決できず、そのレイヤを全面透明として開く (自前のリーダは読める)。
      if (!done)
        for (const auto &o : blobOver)
          if (o.first == cols[i]) {
            sqlite3_bind_blob(dst, at, o.second.data(), int(o.second.size()),
                              SQLITE_TRANSIENT);
            done = true;
          }
      if (!done)
        for (const auto &o : txtOver)
          if (o.first == cols[i]) {
            sqlite3_bind_text(dst, at, o.second.c_str(), int(o.second.size()),
                              SQLITE_TRANSIENT);
            done = true;
          }
      if (!done)
        for (const auto &o : nullOver)
          if (o == cols[i]) { sqlite3_bind_null(dst, at); done = true; }
      if (!done) sqlite3_bind_value(dst, at, sqlite3_column_value(src, int(i)));
    }
    const int rc = sqlite3_step(dst);
    sqlite3_finalize(dst);
    sqlite3_finalize(src);
    if (rc != SQLITE_DONE) {
      error_ = sqlite3_errmsg(db_);
      return false;
    }
    return true;
  }

  // ---- W1: 属性 -----------------------------------------------------------

  bool ClipWriter::setLayerAttr(int64_t layerMainId, const LayerAttr &a) {
    bool found = false;
    queryInt(db_, "SELECT MainId FROM Layer WHERE MainId=?", layerMainId, &found);
    if (!found) { error_ = "no such layer"; return false; }

    struct { const char *col; int64_t v; } ints[] = {
      { "LayerOpacity",    a.opacity },
      { "LayerVisibility", a.visibility },
      { "LayerComposite",  a.composite },
      { "LayerClip",       a.clipping },
      { "LayerFolder",     a.folder },
    };
    for (const auto &e : ints) {
      if (e.v < 0) continue;
      std::string sql = std::string("UPDATE Layer SET [") + e.col +
                        "]=? WHERE MainId=?";
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return false;
      sqlite3_bind_int64(st, 1, e.v);
      sqlite3_bind_int64(st, 2, layerMainId);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    if (a.name) {
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, "UPDATE Layer SET LayerName=? WHERE MainId=?",
                             -1, &st, nullptr) != SQLITE_OK) return false;
      sqlite3_bind_text(st, 1, a.name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 2, layerMainId);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    return true;
  }

  // ---- 100% ミップの Offscreen ---------------------------------------------

  bool ClipWriter::topOffscreenOf(int64_t layerMainId, int64_t &offscreenId,
                                  std::vector<uint8_t> &attrBlob,
                                  OffscreenAttr &attr) {
    const int64_t mip = queryInt(db_, "SELECT LayerRenderMipmap FROM Layer"
                                      " WHERE MainId=?", layerMainId);
    if (!mip) { error_ = "layer has no render mipmap"; return false; }
    const int64_t base = queryInt(db_, "SELECT BaseMipmapInfo FROM Mipmap"
                                       " WHERE MainId=?", mip);
    offscreenId = queryInt(db_, "SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                           base);
    if (!offscreenId) { error_ = "no 100% mipmap level"; return false; }
    if (!queryBlob(db_, "SELECT Attribute FROM Offscreen WHERE MainId=?",
                   offscreenId, attrBlob)) {
      error_ = "offscreen has no Attribute";
      return false;
    }
    if (!parseAttribute(attrBlob.data(), int(attrBlob.size()), attr)) {
      error_ = "cannot parse Attribute";
      return false;
    }
    return true;
  }

  // ---- W2: 画素 -----------------------------------------------------------

  bool ClipWriter::setPixels(int64_t layerMainId, const uint8_t *rgba,
                             uint32_t w, uint32_t h) {
    int64_t offs = 0;
    std::vector<uint8_t> attrBlob;
    OffscreenAttr attr;
    if (!topOffscreenOf(layerMainId, offs, attrBlob, attr)) return false;
    if (attr.numChannels != 4) { error_ = "only RGBA planes supported"; return false; }
    if (w != attr.width || h != attr.height) {
      error_ = "pixel size does not match the 100% mipmap";
      return false;
    }

    std::vector<uint8_t> payload;
    std::vector<uint32_t> sizes;
    if (!buildChunkPayload(rgba, w, h, attr, payload, sizes)) {
      error_ = "buildChunkPayload failed";
      return false;
    }
    if (!patchBlockSizes(attrBlob, sizes)) { error_ = "patchBlockSizes failed"; return false; }

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Offscreen SET Attribute=? WHERE MainId=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_blob(st, 1, attrBlob.data(), int(attrBlob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, offs);
    sqlite3_step(st);
    sqlite3_finalize(st);

    std::string id = queryText(db_, "SELECT BlockData FROM Offscreen WHERE MainId=?",
                               offs);
    if (id.empty()) { error_ = "offscreen has no BlockData id"; return false; }
    replaceExternal(id, std::move(payload));

    dropThumbnail(layerMainId);       // 古いサムネイルが残ると CSP は作り直さない
    return true;
  }

  // ---- サムネイル ---------------------------------------------------------

  bool ClipWriter::dropThumbnail(int64_t layerMainId) {
    const int64_t tn = queryInt(db_, "SELECT LayerRenderThumbnail FROM Layer"
                                     " WHERE MainId=?", layerMainId);
    if (!tn) return false;
    const int64_t off = queryInt(db_, "SELECT ThumbnailOffscreen FROM LayerThumbnail"
                                      " WHERE MainId=?", tn);
    if (off) {
      const std::string id = queryText(db_, "SELECT BlockData FROM Offscreen"
                                            " WHERE MainId=?", off);
      if (!id.empty()) removeExternal(id);
    }
    // `Thumbnail*NeedRefresh` は 0/1 のフラグではなく**世代番号**。
    // CSP が新しく足したレイヤには 50 が入っていた [実測: addlayer_csp.clip]。
    std::string sql = "UPDATE LayerThumbnail SET ";
    bool first = true;
    for (const std::string &c : tableColumns(db_, "LayerThumbnail")) {
      if (c.find("NeedRefresh") == std::string::npos) continue;
      sql += (first ? "" : ", ") + std::string("[") + c + "]=50";
      first = false;
    }
    if (first) return true;
    sql += " WHERE MainId=?";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
      return false;
    sqlite3_bind_int64(st, 1, tn);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
  }

  // ---- W3: レイヤ追加 ------------------------------------------------------

  int64_t ClipWriter::addLayer(int64_t copyFrom, const std::string &name,
                               const uint8_t *rgba, uint32_t w, uint32_t h,
                               int64_t after, int64_t parent) {
    const int64_t srcMipmap = queryInt(db_, "SELECT LayerRenderMipmap FROM Layer"
                                            " WHERE MainId=?", copyFrom);
    const int64_t srcThumb = queryInt(db_, "SELECT LayerRenderThumbnail FROM Layer"
                                           " WHERE MainId=?", copyFrom);
    if (!srcMipmap) { error_ = "template layer not found"; return 0; }

    const int64_t canvasId = queryInt(db_, "SELECT MainId FROM Canvas");
    const int64_t root = queryInt(db_, "SELECT CanvasRootFolder FROM Canvas");
    if (!parent) parent = root;

    const int64_t newLayer = nextId("Layer");
    const int64_t newMipmap = nextId("Mipmap");
    const int64_t newThumb = nextId("LayerThumbnail");

    // --- ミップ連鎖を複製する ---
    std::vector<int64_t> oldInfos, oldOffs;
    for (int64_t node = queryInt(db_, "SELECT BaseMipmapInfo FROM Mipmap"
                                      " WHERE MainId=?", srcMipmap);
         node;) {
      oldInfos.push_back(node);
      oldOffs.push_back(queryInt(db_, "SELECT Offscreen FROM MipmapInfo"
                                      " WHERE MainId=?", node));
      node = queryInt(db_, "SELECT NextIndex FROM MipmapInfo WHERE MainId=?", node);
    }
    std::vector<int64_t> newInfos, newOffs;
    for (size_t i = 0; i < oldInfos.size(); ++i) {
      newOffs.push_back(nextId("Offscreen"));
      newInfos.push_back(nextId("MipmapInfo"));
    }
    for (size_t i = 0; i < oldInfos.size(); ++i) {
      if (!copyRow("Offscreen", "MainId", oldOffs[i],
                   { { "BlockData", newExternalId() } }, {},
                   { { "MainId", newOffs[i] }, { "LayerId", newLayer },
                     { "CanvasId", canvasId } }, {}))
        return 0;
      if (!copyRow("MipmapInfo", "MainId", oldInfos[i], {}, {},
                   { { "MainId", newInfos[i] }, { "LayerId", newLayer },
                     { "CanvasId", canvasId }, { "Offscreen", newOffs[i] },
                     { "NextIndex", i + 1 < newInfos.size() ? newInfos[i + 1] : 0 } },
                   {}))
        return 0;
    }
    if (!copyRow("Mipmap", "MainId", srcMipmap, {}, {},
                 { { "MainId", newMipmap }, { "LayerId", newLayer },
                   { "CanvasId", canvasId }, { "BaseMipmapInfo", newInfos[0] },
                   { "MipmapCount", int64_t(newInfos.size()) } }, {}))
      return 0;

    // --- サムネイル (画素は入れない。CSP が作り直す) ---
    int64_t newThumbOff = 0;
    if (srcThumb) {
      const int64_t oldOff = queryInt(db_, "SELECT ThumbnailOffscreen"
                                           " FROM LayerThumbnail WHERE MainId=?",
                                      srcThumb);
      newThumbOff = nextId("Offscreen");
      if (!copyRow("Offscreen", "MainId", oldOff,
                   { { "BlockData", newExternalId() } }, {},
                   { { "MainId", newThumbOff }, { "LayerId", newLayer },
                     { "CanvasId", canvasId } }, {}))
        return 0;
      std::vector<std::pair<std::string, int64_t>> tnOver = {
        { "MainId", newThumb }, { "LayerId", newLayer }, { "CanvasId", canvasId },
        { "ThumbnailOffscreen", newThumbOff },
      };
      for (const std::string &c : tableColumns(db_, "LayerThumbnail"))
        if (c.find("NeedRefresh") != std::string::npos) tnOver.push_back({ c, 50 });
      if (!copyRow("LayerThumbnail", "MainId", srcThumb, {}, {}, tnOver, {}))
        return 0;
    }

    // --- Layer 行 ---
    if (!copyRow("Layer", "MainId", copyFrom, {},
                 { { "LayerName", name } },
                 { { "MainId", newLayer }, { "CanvasId", canvasId },
                   { "LayerFirstChildIndex", 0 }, { "LayerNextIndex", 0 },
                   { "LayerRenderMipmap", newMipmap },
                   { "LayerRenderThumbnail", srcThumb ? newThumb : 0 },
                   { "LayerLayerMaskMipmap", 0 }, { "LayerLayerMaskThumbnail", 0 },
                   { "LayerSelect", 0 }, { "LayerVisibility", 1 } },
                 { "LightTableInfo" }))   // CSP の新規レイヤは NULL [実測]
      return 0;

    // --- 兄弟チェーンへ繋ぐ ---
    if (after < 0) {
      after = 0;
      for (int64_t node = queryInt(db_, "SELECT LayerFirstChildIndex FROM Layer"
                                        " WHERE MainId=?", parent);
           node;) {
        after = node;
        node = queryInt(db_, "SELECT LayerNextIndex FROM Layer WHERE MainId=?", node);
      }
    }
    sqlite3_stmt *st = nullptr;
    if (after) {
      const int64_t nxt = queryInt(db_, "SELECT LayerNextIndex FROM Layer"
                                        " WHERE MainId=?", after);
      if (sqlite3_prepare_v2(db_, "UPDATE Layer SET LayerNextIndex=? WHERE MainId=?",
                             -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, newLayer); sqlite3_bind_int64(st, 2, after);
        sqlite3_step(st); sqlite3_finalize(st);
      }
      if (sqlite3_prepare_v2(db_, "UPDATE Layer SET LayerNextIndex=? WHERE MainId=?",
                             -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, nxt); sqlite3_bind_int64(st, 2, newLayer);
        sqlite3_step(st); sqlite3_finalize(st);
      }
    } else {
      if (sqlite3_prepare_v2(db_, "UPDATE Layer SET LayerFirstChildIndex=?"
                                  " WHERE MainId=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, newLayer); sqlite3_bind_int64(st, 2, parent);
        sqlite3_step(st); sqlite3_finalize(st);
      }
    }

    // --- 100% ミップの画素 ---
    std::vector<uint8_t> attrBlob;
    OffscreenAttr attr;
    if (!queryBlob(db_, "SELECT Attribute FROM Offscreen WHERE MainId=?",
                   newOffs[0], attrBlob) ||
        !parseAttribute(attrBlob.data(), int(attrBlob.size()), attr)) {
      error_ = "cannot read new Attribute";
      return 0;
    }
    std::vector<uint8_t> transparent;
    if (!rgba) {
      transparent.assign(size_t(attr.width) * attr.height * 4, 0);
      rgba = transparent.data();
      w = attr.width; h = attr.height;
    }
    if (w != attr.width || h != attr.height) {
      error_ = "pixel size does not match the 100% mipmap";
      return 0;
    }
    std::vector<uint8_t> payload;
    std::vector<uint32_t> sizes;
    if (!buildChunkPayload(rgba, w, h, attr, payload, sizes) ||
        !patchBlockSizes(attrBlob, sizes)) {
      error_ = "buildChunkPayload failed";
      return 0;
    }
    if (sqlite3_prepare_v2(db_, "UPDATE Offscreen SET Attribute=? WHERE MainId=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_blob(st, 1, attrBlob.data(), int(attrBlob.size()), SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 2, newOffs[0]);
      sqlite3_step(st); sqlite3_finalize(st);
    }
    const std::string id = queryText(db_, "SELECT BlockData FROM Offscreen"
                                          " WHERE MainId=?", newOffs[0]);
    replaceExternal(id, std::move(payload));
    return newLayer;
  }

  // ---- レイヤ削除 ---------------------------------------------------------

  bool ClipWriter::deleteLayer(int64_t layerMainId) {
    bool found = false;
    const int64_t mip = queryInt(db_, "SELECT LayerRenderMipmap FROM Layer"
                                      " WHERE MainId=?", layerMainId, &found);
    if (!found) { error_ = "no such layer"; return false; }
    const int64_t tn = queryInt(db_, "SELECT LayerRenderThumbnail FROM Layer"
                                     " WHERE MainId=?", layerMainId);

    std::vector<int64_t> deadOffs;
    for (int64_t node = mip ? queryInt(db_, "SELECT BaseMipmapInfo FROM Mipmap"
                                            " WHERE MainId=?", mip) : 0;
         node;) {
      deadOffs.push_back(queryInt(db_, "SELECT Offscreen FROM MipmapInfo"
                                       " WHERE MainId=?", node));
      const int64_t nxt = queryInt(db_, "SELECT NextIndex FROM MipmapInfo"
                                        " WHERE MainId=?", node);
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, "DELETE FROM MipmapInfo WHERE MainId=?",
                             -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, node); sqlite3_step(st); sqlite3_finalize(st);
      }
      node = nxt;
    }
    auto del = [&](const char *sql, int64_t id) {
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
      sqlite3_bind_int64(st, 1, id); sqlite3_step(st); sqlite3_finalize(st);
    };
    if (mip) del("DELETE FROM Mipmap WHERE MainId=?", mip);
    if (tn) {
      const int64_t off = queryInt(db_, "SELECT ThumbnailOffscreen FROM LayerThumbnail"
                                        " WHERE MainId=?", tn);
      if (off) deadOffs.push_back(off);
      del("DELETE FROM LayerThumbnail WHERE MainId=?", tn);
    }
    for (int64_t off : deadOffs) {
      if (!off) continue;
      const std::string id = queryText(db_, "SELECT BlockData FROM Offscreen"
                                            " WHERE MainId=?", off);
      if (!id.empty()) removeExternal(id);
      del("DELETE FROM Offscreen WHERE MainId=?", off);
    }

    // 兄弟チェーンから外す
    const int64_t nxt = queryInt(db_, "SELECT LayerNextIndex FROM Layer"
                                      " WHERE MainId=?", layerMainId);
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Layer SET LayerNextIndex=?"
                                " WHERE LayerNextIndex=?", -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, nxt); sqlite3_bind_int64(st, 2, layerMainId);
      sqlite3_step(st); sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db_, "UPDATE Layer SET LayerFirstChildIndex=?"
                                " WHERE LayerFirstChildIndex=?", -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, nxt); sqlite3_bind_int64(st, 2, layerMainId);
      sqlite3_step(st); sqlite3_finalize(st);
    }
    del("DELETE FROM Layer WHERE MainId=?", layerMainId);

    // **選択中レイヤが消えたレイヤを指したままにしない**
    const int64_t canvasId = queryInt(db_, "SELECT MainId FROM Canvas");
    const int64_t root = queryInt(db_, "SELECT CanvasRootFolder FROM Canvas");
    if (queryInt(db_, "SELECT CanvasCurrentLayer FROM Canvas") == layerMainId) {
      int64_t top = 0;
      for (int64_t node = queryInt(db_, "SELECT LayerFirstChildIndex FROM Layer"
                                        " WHERE MainId=?", root);
           node;) {
        top = node;
        node = queryInt(db_, "SELECT LayerNextIndex FROM Layer WHERE MainId=?", node);
      }
      if (sqlite3_prepare_v2(db_, "UPDATE Canvas SET CanvasCurrentLayer=?"
                                  " WHERE MainId=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, top ? top : root);
        sqlite3_bind_int64(st, 2, canvasId);
        sqlite3_step(st); sqlite3_finalize(st);
      }
    }
    return true;
  }

  // ---- CanvasPreview -------------------------------------------------------

  bool ClipWriter::setCanvasPreview(const uint8_t *rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> png;
    if (!encodePng(rgba, w, h, png)) { error_ = "encodePng failed"; return false; }
    const int64_t canvasId = queryInt(db_, "SELECT MainId FROM Canvas");
    bool found = false;
    const int64_t id = queryInt(db_, "SELECT MainId FROM CanvasPreview", -1, &found);

    sqlite3_stmt *st = nullptr;
    if (found) {
      if (sqlite3_prepare_v2(db_, "UPDATE CanvasPreview SET ImageType=1,"
                                  " ImageWidth=?, ImageHeight=?, ImageData=?"
                                  " WHERE MainId=?", -1, &st, nullptr) != SQLITE_OK)
        return false;
      sqlite3_bind_int64(st, 1, w);
      sqlite3_bind_int64(st, 2, h);
      sqlite3_bind_blob(st, 3, png.data(), int(png.size()), SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 4, id);
    } else {
      if (sqlite3_prepare_v2(db_, "INSERT INTO CanvasPreview (MainId, CanvasId,"
                                  " ImageType, ImageWidth, ImageHeight, ImageData)"
                                  " VALUES (1, ?, 1, ?, ?, ?)",
                             -1, &st, nullptr) != SQLITE_OK) return false;
      sqlite3_bind_int64(st, 1, canvasId);
      sqlite3_bind_int64(st, 2, w);
      sqlite3_bind_int64(st, 3, h);
      sqlite3_bind_blob(st, 4, png.data(), int(png.size()), SQLITE_TRANSIENT);
    }
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
  }

  // ---- キャンバスの寸法を作り替える ---------------------------------------

  bool ClipWriter::retargetChain(int64_t canvasId, int64_t mipmapId,
                                 const std::vector<std::pair<uint32_t, uint32_t>> &levels) {
    std::vector<int64_t> infos, offs;
    for (int64_t node = queryInt(db_, "SELECT BaseMipmapInfo FROM Mipmap"
                                      " WHERE MainId=?", mipmapId);
         node;) {
      infos.push_back(node);
      offs.push_back(queryInt(db_, "SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                              node));
      node = queryInt(db_, "SELECT NextIndex FROM MipmapInfo WHERE MainId=?", node);
    }
    if (infos.empty()) return true;
    const int64_t layerId = queryInt(db_, "SELECT LayerId FROM MipmapInfo"
                                          " WHERE MainId=?", infos[0]);

    auto del = [&](const char *sql, int64_t id) {
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
      sqlite3_bind_int64(st, 1, id); sqlite3_step(st); sqlite3_finalize(st);
    };
    while (infos.size() > levels.size()) {
      del("DELETE FROM MipmapInfo WHERE MainId=?", infos.back());
      del("DELETE FROM Offscreen WHERE MainId=?", offs.back());
      infos.pop_back(); offs.pop_back();
    }
    while (infos.size() < levels.size()) {
      const int64_t newOff = nextId("Offscreen"), newInfo = nextId("MipmapInfo");
      if (!copyRow("Offscreen", "MainId", offs.back(),
                   { { "BlockData", newExternalId() } }, {},
                   { { "MainId", newOff } }, {})) return false;
      if (!copyRow("MipmapInfo", "MainId", infos.back(), {}, {},
                   { { "MainId", newInfo }, { "Offscreen", newOff },
                     { "NextIndex", 0 } }, {})) return false;
      infos.push_back(newInfo); offs.push_back(newOff);
    }

    for (size_t i = 0; i < infos.size(); ++i) {
      const double scale = 100.0 / double(int64_t(1) << i);
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db_, "UPDATE MipmapInfo SET ThisScale=?, NextIndex=?,"
                                  " CanvasId=?, LayerId=? WHERE MainId=?",
                             -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_double(st, 1, scale);
        sqlite3_bind_int64(st, 2, i + 1 < infos.size() ? infos[i + 1] : 0);
        sqlite3_bind_int64(st, 3, canvasId);
        sqlite3_bind_int64(st, 4, layerId);
        sqlite3_bind_int64(st, 5, infos[i]);
        sqlite3_step(st); sqlite3_finalize(st);
      }
      std::vector<uint8_t> attr, out;
      if (!queryBlob(db_, "SELECT Attribute FROM Offscreen WHERE MainId=?",
                     offs[i], attr)) continue;
      if (!retargetAttribute(attr.data(), int(attr.size()), levels[i].first,
                             levels[i].second, nullptr, out)) {
        error_ = "retargetAttribute failed";
        return false;
      }
      if (sqlite3_prepare_v2(db_, "UPDATE Offscreen SET Attribute=?, CanvasId=?,"
                                  " LayerId=? WHERE MainId=?",
                             -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, out.data(), int(out.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, canvasId);
        sqlite3_bind_int64(st, 3, layerId);
        sqlite3_bind_int64(st, 4, offs[i]);
        sqlite3_step(st); sqlite3_finalize(st);
      }
    }
    // **MipmapCount を段数に合わせる**。違うと CSP が読み込み中に落ちる。
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Mipmap SET BaseMipmapInfo=?, MipmapCount=?,"
                                " CanvasId=?, LayerId=? WHERE MainId=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, infos[0]);
      sqlite3_bind_int64(st, 2, int64_t(infos.size()));
      sqlite3_bind_int64(st, 3, canvasId);
      sqlite3_bind_int64(st, 4, layerId);
      sqlite3_bind_int64(st, 5, mipmapId);
      sqlite3_step(st); sqlite3_finalize(st);
    }
    return true;
  }

  bool ClipWriter::resizeCanvas(uint32_t w, uint32_t h, double dpi) {
    const int64_t canvasId = queryInt(db_, "SELECT MainId FROM Canvas");
    const auto levels = mipLevels(w, h);

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE Canvas SET CanvasWidth=?, CanvasHeight=?,"
                                " CanvasUnit=0 WHERE MainId=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_double(st, 1, double(w));
      sqlite3_bind_double(st, 2, double(h));
      sqlite3_bind_int64(st, 3, canvasId);
      sqlite3_step(st); sqlite3_finalize(st);
    }
    if (dpi > 0 &&
        sqlite3_prepare_v2(db_, "UPDATE Canvas SET CanvasResolution=? WHERE MainId=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_double(st, 1, dpi);
      sqlite3_bind_int64(st, 2, canvasId);
      sqlite3_step(st); sqlite3_finalize(st);
    }

    std::vector<int64_t> mipmaps;
    if (sqlite3_prepare_v2(db_, "SELECT MainId FROM Mipmap", -1, &st, nullptr)
        == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW)
        mipmaps.push_back(sqlite3_column_int64(st, 0));
      sqlite3_finalize(st);
    }
    for (int64_t m : mipmaps)
      if (!retargetChain(canvasId, m, levels)) return false;

    // サムネイルは 512x512 固定。中身だけ空にする。
    std::vector<int64_t> thumbs;
    if (sqlite3_prepare_v2(db_, "SELECT o.MainId FROM Offscreen o JOIN LayerThumbnail t"
                                " ON t.ThumbnailOffscreen = o.MainId",
                           -1, &st, nullptr) == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW)
        thumbs.push_back(sqlite3_column_int64(st, 0));
      sqlite3_finalize(st);
    }
    for (int64_t off : thumbs) {
      std::vector<uint8_t> attr, out;
      OffscreenAttr a;
      if (!queryBlob(db_, "SELECT Attribute FROM Offscreen WHERE MainId=?", off, attr))
        continue;
      if (!parseAttribute(attr.data(), int(attr.size()), a)) continue;
      if (!retargetAttribute(attr.data(), int(attr.size()), a.width, a.height,
                             nullptr, out)) continue;
      if (sqlite3_prepare_v2(db_, "UPDATE Offscreen SET Attribute=? WHERE MainId=?",
                             -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, out.data(), int(out.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, off);
        sqlite3_step(st); sqlite3_finalize(st);
      }
    }
    externals_.clear();       // 実体は全部作り直し
    return true;
  }

} // namespace clip
