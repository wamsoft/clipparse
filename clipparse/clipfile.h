#ifndef __clipfile_h__
#define __clipfile_h__

// CLIP STUDIO PAINT (.clip) の遅延参照リーダ。
//
//   load(path)  : ファイルを mmap する。メタ情報 (SQLite) だけを触り、
//                 画素は読まない。
//   layers()    : レイヤツリー (SQLite の Layer をリンクリストで解決したもの)
//   readBlock() : 指定ブロックだけを展開する。**他のブロックには触らない**。
//
// 設計の要は「SQLite の ExternalChunk.Offset と Offscreen.Attribute の
// BlockSize[] 前置和だけで、任意ブロックの絶対オフセットが決まる」こと。
// バイナリ領域を走査する必要がない。詳細は docs/DESIGN.md。

#include "clipbase.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace clip {

  // Offscreen.Attribute を解いたもの
  struct OffscreenAttr {
    uint32_t width = 0, height = 0;      // 論理サイズ
    uint32_t cols = 0, rows = 0;         // ブロックグリッド
    uint32_t colorMode = 0;              // 33=RGBA / 17=グレー・モノクロ / 1=マスク
    uint32_t numChannels = 0;            // 4 / 1 / 0
    uint32_t bitDepth = 0;               // 5=8bpp RGBA / 2=8bpp / 1=1bpp
    uint32_t planeBytes = 0, planeCount = 0, rowBytes = 0;
    uint32_t blockWidth = 0, blockHeight = 0;
    bool     hasInitColor = false;
    uint32_t initColor = 0;              // RGBA を BE パックしたもの
    std::vector<uint32_t> blockSizes;    // サブレコード全長 (圧縮長ではない)
    std::vector<uint64_t> blockOffsets;  // blockSizes の前置和
    std::string blockDataId;             // external_id
  };

  struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
  };

  // 画素取得モード (psdparse の ImageMode に合わせてある)
  enum ImageMode {
    IMAGE_MODE_IMAGE,        // マスクを繰り込まない画像
    IMAGE_MODE_MASK,         // マスクのみ (グレー)
    IMAGE_MODE_MASKED,       // マスクをアルファに繰り込んだ画像
  };

  struct LayerInfo {
    int64_t     mainId = 0;
    std::string name;
    int64_t     type = 0;                // bit12(4096)=調整レイヤ, bit1(2)=マスク有
    int64_t     folder = 0;              // bit0 = フォルダ, bit4 = 折り畳み
    int64_t     visibility = 1;
    int64_t     opacity = 256;           // 0..256 (**255 ではない**)
    int64_t     composite = 0;           // LayerComposite
    int64_t     clipping = 0;
    int64_t     renderMipmap = 0, maskMipmap = 0;
    int64_t     renderThumbnail = 0, maskThumbnail = 0;

    // layers() は **psdparse と同じ平坦順** (中身が先、フォルダが後、下から上)。
    // parent / children はその index を指す。
    int         parent = -1;             // -1 = 最上位
    std::vector<int> children;

    Rect        bounds;                  // キャンバス上で画素が占める矩形
    bool        isGroup = false;
    bool        isFilter = false;        // 調整レイヤ
    bool        isText = false;
    bool        hasMask = false;
  };

  // 展開したブロック 1 枚 (常に RGBA8、幅 blockWidth・高さ blockHeight)
  struct Block {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;           // width*height*4
    bool empty = true;                   // 実体を持たないブロック
  };

  // 画像 1 枚 (RGBA8 ストレートアルファ)
  struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    void resize(uint32_t w, uint32_t h) {
      width = w; height = h;
      rgba.assign(size_t(w) * h * 4, 0);
    }
    uint8_t *at(uint32_t x, uint32_t y) { return &rgba[(size_t(y) * width + x) * 4]; }
    const uint8_t *at(uint32_t x, uint32_t y) const {
      return &rgba[(size_t(y) * width + x) * 4];
    }
  };

  static const int64_t FILTER_BIT = 4096;   // LayerType: 調整レイヤ

  class ClipFile {
  public:
    ClipFile();
    ~ClipFile();

    // filename は UTF-8。Win32 では内部で UTF-16 に変換して mmap する。
    bool load(const char *filename);
    // 呼び出し元のバイト列を参照する (所有しない)。
    bool loadFromMemory(const uint8_t *data, int64_t size);
    void clear();

    const std::string &error() const { return error_; }

    // --- メタ情報 ---
    // キャンバスの**実ピクセル**寸法。`Canvas.CanvasWidth/Height` は
    // `CanvasUnit` の単位で、mm のファイルが実在するので使えない。
    // ルートフォルダの 100% ミップの Attribute から取る。
    int64_t canvasWidth() const { return canvasPixelW_; }
    int64_t canvasHeight() const { return canvasPixelH_; }
    double  canvasResolution() const { return canvasRes_; }
    const std::vector<int> &roots() const { return rootChildren_; }
    int64_t rootLayer() const { return rootLayer_; }
    const std::vector<LayerInfo> &layers() const { return layers_; }
    int layerIndex(int64_t mainId) const;

    // 描画用 / マスク用の 100% ミップの Offscreen.MainId。無ければ 0。
    int64_t topOffscreen(int64_t layerMainId, bool mask = false) const;
    // ミップ段でもサムネイルでもない Offscreen (テキスト等の外接矩形ラスタ)
    int64_t objectOffscreen(int64_t layerMainId) const;

    // Offscreen.Attribute を解く (結果はキャッシュ)
    const OffscreenAttr *attribute(int64_t offscreenId) const;
    bool hasPixels(int64_t offscreenId) const;

    // --- 実データ (ここで初めてバイナリ領域を触る) ---
    bool readBlock(int64_t offscreenId, uint32_t blockIndex, Block &out) const;
    // offscreen 全面を RGBA8 で組み立てる (初期色があれば下地に敷く)
    bool readOffscreen(int64_t offscreenId, std::vector<uint8_t> &rgba,
                       uint32_t &w, uint32_t &h) const;
    bool readOffscreen(int64_t offscreenId, Image &out) const;

    // offscreen の一部だけを読む。**必要なブロックしか展開しない。**
    // CLIP は 256x256 タイルなので、大きなレイヤの一部を取るのが安い。
    bool readOffscreenRegion(int64_t offscreenId, const Rect &r, Image &out) const;

    // --- レイヤ単位の画像 ---
    //
    // 戻る画像は `layers()[index].bounds` の大きさ。フォルダは空 (0x0)。
    bool layerImage(int index, ImageMode mode, Image &out) const;
    // レイヤ画像のうち rect (キャンバス座標) の部分だけを読む。
    bool layerRegion(int index, const Rect &r, ImageMode mode, Image &out) const;

    // 全レイヤを下から合成する。CSP の CanvasPreview と一致するのが正。
    bool mergedImage(Image &out) const;

    // ファイルに埋まっているプレビュー画像 (CanvasPreview) を PNG バイト列で返す。
    // CSP が保存した完成画そのもの。等倍とは限らない。
    bool previewPng(std::vector<uint8_t> &png, int &w, int &h) const;

    // 生 SQLite ハンドル (モデル化していない列に届くための逃げ道)
    sqlite3 *db() const { return db_; }

    // 診断用: 全ブロックの構造アサーションを回す。問題があれば false。
    bool checkAll(std::string *report = nullptr) const;

  private:
    bool parse();
    bool openDb();
    bool hasColumn(const char *table, const char *column) const;
    bool layerHasText(int64_t mainId) const;
    bool textOrigin(int64_t mainId, uint32_t w, uint32_t h, int &x, int &y) const;
    Rect computeBounds(const LayerInfo &li) const;
    // 合成 (clipcomposite.cpp)
    bool compositeInto(int64_t parentMainId, Image &dst) const;
    bool layerPixels(const LayerInfo &li, Image &out) const;
    void applyFilter(Image &dst, const uint8_t *blob, int len) const;

    struct Mapping;
    std::unique_ptr<Mapping> mapping_;
    std::vector<uint8_t> owned_;
    std::unique_ptr<MemorySource> source_;

    sqlite3 *db_ = nullptr;
    int64_t  dbOffset_ = 0, dbSize_ = 0;

    std::map<std::string, int64_t> extOffset_;   // external_id → CHNKExta の絶対位置
    std::vector<LayerInfo> layers_;
    std::map<int64_t, int> layerById_;
    int64_t canvasW_ = 0, canvasH_ = 0, rootLayer_ = 0;
    int64_t canvasPixelW_ = 0, canvasPixelH_ = 0;
    double  canvasRes_ = 72.0;
    std::vector<int> rootChildren_;
    bool    hasTextColumn_ = false;

    mutable std::map<int64_t, OffscreenAttr> attrCache_;
    std::string error_;
  };

} // namespace clip

#endif
