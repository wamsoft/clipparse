// `.clip` の参照整合性を検査する (tools/clip_validate.py の C++ 版)。
//
// `ClipFile::checkAll` はブロックの読み出しを検査するが、こちらは
// **SQLite の中の参照が全部生きているか**を見る。CSP で開く前に通すこと。
// ここに引っかかる種類の間違いは、寛容なリーダでは読めてしまうのに
// CSP では落ちたり全面透明になったりする。

#include "clipwrite.h"
#include "sqlite3.h"

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <set>
#include <string>

namespace clip {

  namespace {

    std::set<int64_t> idSet(sqlite3 *db, const char *sql) {
      std::set<int64_t> out;
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
      while (sqlite3_step(st) == SQLITE_ROW) out.insert(sqlite3_column_int64(st, 0));
      sqlite3_finalize(st);
      return out;
    }

    int64_t one(sqlite3 *db, const char *sql, int64_t arg = -1) {
      sqlite3_stmt *st = nullptr;
      int64_t v = 0;
      if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
      if (arg >= 0) sqlite3_bind_int64(st, 1, arg);
      if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
      sqlite3_finalize(st);
      return v;
    }

    std::string fmt(const char *f, ...) {
      char buf[512];
      va_list ap;
      va_start(ap, f);
      vsnprintf(buf, sizeof buf, f, ap);
      va_end(ap);
      return buf;
    }

  } // namespace

  bool validate(const char *path, std::vector<std::string> &problems) {
    problems.clear();
    ClipWriter w;
    if (!w.load(path)) {
      problems.push_back("cannot load: " + w.error());
      return false;
    }
    sqlite3 *db = w.db();

    const std::set<int64_t> layers = idSet(db, "SELECT MainId FROM Layer");
    const std::set<int64_t> mipmaps = idSet(db, "SELECT MainId FROM Mipmap");
    const std::set<int64_t> infos = idSet(db, "SELECT MainId FROM MipmapInfo");
    const std::set<int64_t> offs = idSet(db, "SELECT MainId FROM Offscreen");
    const std::set<int64_t> thumbs = idSet(db, "SELECT MainId FROM LayerThumbnail");

    const int64_t root = one(db, "SELECT CanvasRootFolder FROM Canvas");
    const int64_t cur = one(db, "SELECT CanvasCurrentLayer FROM Canvas");
    if (!layers.count(root))
      problems.push_back(fmt("Canvas.CanvasRootFolder=%lld is not in Layer",
                             (long long)root));
    if (cur && !layers.count(cur))
      problems.push_back(fmt("Canvas.CanvasCurrentLayer=%lld is not in Layer"
                             " (points at a deleted layer)", (long long)cur));

    // --- ツリー: 閉路と到達性 ---
    std::set<int64_t> seen;
    std::function<void(int64_t)> walk = [&](int64_t node) {
      while (node) {
        if (seen.count(node)) {
          problems.push_back(fmt("layer sibling chain loops at #%lld", (long long)node));
          return;
        }
        if (!layers.count(node)) {
          problems.push_back(fmt("chain points at missing layer #%lld", (long long)node));
          return;
        }
        seen.insert(node);
        const int64_t kid = one(db, "SELECT LayerFirstChildIndex FROM Layer"
                                    " WHERE MainId=?", node);
        const int64_t nxt = one(db, "SELECT LayerNextIndex FROM Layer WHERE MainId=?",
                                node);
        if (kid) walk(kid);
        node = nxt;
      }
    };
    if (layers.count(root)) {
      seen.insert(root);
      walk(one(db, "SELECT LayerFirstChildIndex FROM Layer WHERE MainId=?", root));
    }
    for (int64_t id : layers)
      if (!seen.count(id))
        problems.push_back(fmt("layer #%lld is unreachable from the tree",
                               (long long)id));

    // --- ミップ連鎖 ---
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT MainId, LayerRenderMipmap,"
                               " LayerRenderThumbnail FROM Layer",
                           -1, &st, nullptr) == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW) {
        const int64_t lid = sqlite3_column_int64(st, 0);
        const int64_t mm = sqlite3_column_int64(st, 1);
        const int64_t tn = sqlite3_column_int64(st, 2);
        if (mm && !mipmaps.count(mm)) {
          problems.push_back(fmt("Layer #%lld.LayerRenderMipmap=%lld is not in Mipmap",
                                 (long long)lid, (long long)mm));
        } else if (mm) {
          int hops = 0;
          const int64_t count = one(db, "SELECT MipmapCount FROM Mipmap WHERE MainId=?",
                                    mm);
          for (int64_t node = one(db, "SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                                  mm);
               node;) {
            if (!infos.count(node)) {
              problems.push_back(fmt("Layer #%lld mipmap chain breaks at MipmapInfo"
                                     " #%lld", (long long)lid, (long long)node));
              break;
            }
            const int64_t o = one(db, "SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                                  node);
            if (!offs.count(o))
              problems.push_back(fmt("MipmapInfo #%lld.Offscreen=%lld is missing",
                                     (long long)node, (long long)o));
            node = one(db, "SELECT NextIndex FROM MipmapInfo WHERE MainId=?", node);
            if (++hops > 64) {
              problems.push_back(fmt("Layer #%lld mipmap chain does not terminate",
                                     (long long)lid));
              break;
            }
          }
          // **段数と食い違うと CSP が読み込み中に落ちる**
          if (count != hops)
            problems.push_back(fmt("Mipmap #%lld.MipmapCount=%lld != actual levels %d"
                                   " (CSP walks past the end and crashes)",
                                   (long long)mm, (long long)count, hops));
        }
        if (tn && !thumbs.count(tn)) {
          problems.push_back(fmt("Layer #%lld.LayerRenderThumbnail=%lld is missing",
                                 (long long)lid, (long long)tn));
        } else if (tn) {
          const int64_t o = one(db, "SELECT ThumbnailOffscreen FROM LayerThumbnail"
                                    " WHERE MainId=?", tn);
          if (!offs.count(o))
            problems.push_back(fmt("LayerThumbnail #%lld.ThumbnailOffscreen=%lld"
                                   " is missing", (long long)tn, (long long)o));
        }
      }
      sqlite3_finalize(st);
    }

    // --- 孤児行 ---
    const char *tables[] = { "Offscreen", "Mipmap", "MipmapInfo", "LayerThumbnail" };
    for (const char *t : tables) {
      const std::string sql = std::string("SELECT COUNT(*) FROM [") + t +
                              "] WHERE LayerId NOT IN (SELECT MainId FROM Layer)";
      const int64_t n = one(db, sql.c_str());
      if (n)
        problems.push_back(fmt("%s has %lld orphan rows (LayerId not in Layer)",
                               t, (long long)n));
    }

    // --- 格納型 ---
    // **同じ ID なのにテーブルごとに BLOB / TEXT と違う**。取り違えると
    // CSP は実体を解決できず、そのレイヤを全面透明として開く。
    {
      std::set<std::string> kinds;
      if (sqlite3_prepare_v2(db, "SELECT DISTINCT typeof(BlockData) FROM Offscreen",
                             -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
          kinds.insert((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
      }
      if (kinds.size() != 1 || !kinds.count("blob"))
        problems.push_back("Offscreen.BlockData must be stored as blob");

      kinds.clear();
      if (sqlite3_prepare_v2(db, "SELECT DISTINCT typeof(ExternalID) FROM ExternalChunk",
                             -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
          kinds.insert((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
      }
      if (!kinds.empty() && (kinds.size() != 1 || !kinds.count("text")))
        problems.push_back("ExternalChunk.ExternalID must be stored as text");
    }

    // --- 行数と実チャンク数 ---
    const int64_t rows = one(db, "SELECT COUNT(*) FROM ExternalChunk");
    if (rows != w.externalCount())
      problems.push_back(fmt("ExternalChunk has %lld rows but the file has %lld chunks",
                             (long long)rows, (long long)w.externalCount()));
    return problems.empty();
  }

} // namespace clip
