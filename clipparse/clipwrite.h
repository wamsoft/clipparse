#ifndef __clipwrite_h__
#define __clipwrite_h__

// `.clip` を書く (tools/clip_write.py の C++ 版)。
//
//   ClipWriter w;
//   w.load("in.clip");
//   w.addLayer(3, "追加", rgba, 300, 400);
//   w.save("out.clip");
//
// **設計上の要点**: `CHNKSQLi` はバイナリ領域の後ろにあるので、SQLite だけを
// 書き換える編集ではどの `ExternalChunk.Offset` も動かない。チャンクを増減する
// 編集では、新しいオフセットを先に全部計算してから `ExternalChunk` を更新し、
// 最後に SQLite を直列化する。
//
// **CSP が受け付けるための作法** (どれも自前のリーダでは検出できない。
// 詳細は docs/CLIP_FORMAT.md、検査は clip::validate):
//
//   1. `Offscreen.BlockData` は BLOB、`ExternalChunk.ExternalID` は TEXT
//   2. `BlockCheckSum` は 0 (非ゼロは照合されて「破損」になる)
//   3. `Mipmap.MipmapCount` は必ず段数と一致 (違うと CSP が落ちる)
//   4. サムネイルは実体を消し、`Thumbnail*NeedRefresh` に 50
//   5. `CanvasPreview` を合成し直す
//   6. `Canvas.CanvasCurrentLayer` を生きたレイヤに向ける

#include "clipfile.h"
#include <string>
#include <vector>

struct sqlite3;

namespace clip {

  class ClipWriter {
  public:
    ClipWriter();
    ~ClipWriter();

    // ファイル全体をメモリへ読む (mmap しないので同じパスへ書き戻せる)。
    bool load(const char *path);
    // 現在の状態でファイルを組み立てる。書いたバイト数、失敗時 0。
    int64_t save(const char *path);
    void clear();

    const std::string &error() const { return error_; }
    sqlite3 *db() const { return db_; }
    int64_t externalCount() const { return int64_t(externals_.size()); }

    // --- W1: レイヤ属性 -----------------------------------------------------
    struct LayerAttr {
      const char *name = nullptr;         // nullptr = 変更しない
      int64_t opacity = -1;               // 0..256 (**255 ではない**)
      int64_t visibility = -1;            // 0 / 1
      int64_t composite = -1;             // docs/CLIP_FORMAT.md §9
      int64_t clipping = -1;
      int64_t folder = -1;                // bit0=フォルダ, bit4=折り畳み
    };
    bool setLayerAttr(int64_t layerMainId, const LayerAttr &attr);

    // --- W2: 画素の差し替え -------------------------------------------------
    // rgba はキャンバス全面 (100% ミップと同じ寸法)。
    bool setPixels(int64_t layerMainId, const uint8_t *rgba,
                   uint32_t w, uint32_t h);

    // --- W3: レイヤの追加・削除 ---------------------------------------------
    // 既存レイヤを雛形に複製する。`rgba` は nullptr で透明。
    // `after` < 0 なら最上段、`parent` == 0 ならルート直下。
    int64_t addLayer(int64_t copyFrom, const std::string &name,
                     const uint8_t *rgba, uint32_t w, uint32_t h,
                     int64_t after = -1, int64_t parent = 0);
    bool deleteLayer(int64_t layerMainId);

    // サムネイルの**実体だけ**落とし、世代番号を立てる (CSP が作り直す)。
    bool dropThumbnail(int64_t layerMainId);

    // 開いた直後に表示される画像を差し替える。
    bool setCanvasPreview(const uint8_t *rgba, uint32_t w, uint32_t h);

    // キャンバスの寸法ごと作り替える (ミップ連鎖を伸縮させる)。
    // 実体は全部落ちるので、呼び出し側で入れ直すこと。
    bool resizeCanvas(uint32_t w, uint32_t h, double dpi = 0.0);

    // 外部 ID の乱数種を固定する (テストで再現性が要るとき)。
    void setExternalIdSeed(uint64_t seed);

  private:
    std::string newExternalId();
    int64_t nextId(const char *table);
    // blobOver / txtOver は**格納型が違う**ので分けてある。
    // `Offscreen.BlockData` は BLOB、`ExternalChunk.ExternalID` は TEXT。
    bool copyRow(const char *table, const char *whereCol, int64_t whereVal,
                 const std::vector<std::pair<std::string, std::string>> &blobOver,
                 const std::vector<std::pair<std::string, std::string>> &txtOver,
                 const std::vector<std::pair<std::string, int64_t>> &i64Over,
                 const std::vector<std::string> &nullOver);
    bool topOffscreenOf(int64_t layerMainId, int64_t &offscreenId,
                        std::vector<uint8_t> &attrBlob, OffscreenAttr &attr);
    bool retargetChain(int64_t canvasId, int64_t mipmapId,
                       const std::vector<std::pair<uint32_t, uint32_t>> &levels);
    void replaceExternal(const std::string &id, std::vector<uint8_t> &&payload);
    void removeExternal(const std::string &id);

    std::vector<uint8_t> headBody_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> externals_;
    int64_t headerLen_ = 0;
    int64_t footLen_ = 0;
    sqlite3 *db_ = nullptr;
    uint64_t idState_ = 0;
    bool idSeeded_ = false;
    std::string error_;
  };

  // --- 参照整合性の検査 (tools/clip_validate.py の C++ 版) ------------------
  //
  // CSP で開く前にこれを通すこと。ミップ段数 / 閉路 / 孤児行 / 格納型 /
  // 消えたレイヤへの参照を見る。問題が無ければ true。
  bool validate(const char *path, std::vector<std::string> &problems);

} // namespace clip

#endif
