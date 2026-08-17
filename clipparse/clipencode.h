#ifndef __clipencode_h__
#define __clipencode_h__

// `.clip` を**書く**側の部品 (tools/clip_encode.py + clip_build.py の C++ 版)。
//
// 読む側 (clipfile.cpp) の逆写像。ここは純粋な関数だけで、ファイルや
// SQLite には触らない。組み立ては ClipWriter (clipwrite.h) が行う。
//
// 書く側の落とし穴は docs/CLIP_FORMAT.md にまとめてある。特に:
//   - サブレコード長 = 圧縮長 + 112。空ブロックは 104 バイト固定
//   - 圧縮長の欄**だけ**リトルエンディアン
//   - `BlockCheckSum` は **0** を書く (非ゼロは CSP が照合して弾く)

#include "clipfile.h"
#include <utility>
#include <vector>

namespace clip {

  static const uint32_t ALPHA_TILE = 64;
  static const uint32_t EMPTY_RECORD_SIZE = 104;   // 4+4+38 + 20 + 4+34

  // clipfile.cpp と共有する Attribute パーサ (InitColor が可変長なのに注意)
  bool parseAttribute(const uint8_t *a, int len, OffscreenAttr &out);

  // --- ピクセルブロック ---------------------------------------------------

  // RGBA (bh, bw, 4) → 展開後のブロック 1 枚。
  // rows[64:] が B,G,R,(未使用)、rows[0:64] が 4x4 に畳んだアルファ面。
  void encodeRgbaBlock(const uint8_t *rgba, uint32_t bw, uint32_t bh,
                       std::vector<uint8_t> &out);

  // 自己検証用 (encode の逆)。
  void decodeRgbaBlock(const uint8_t *buf, uint32_t bw, uint32_t bh,
                       std::vector<uint8_t> &rgba);

  // ブロック 1 枚のサブレコード。raw == nullptr なら空ブロック。
  void buildBlockRecord(uint32_t index, const std::vector<uint8_t> *raw,
                        uint32_t bw, uint32_t bh, std::vector<uint8_t> &out);

  // 末尾の BlockStatus / BlockCheckSum。**サイズ前置が無い**のが違い。
  void buildTrailers(const std::vector<uint32_t> &status,
                     const std::vector<uint32_t> &checksum,
                     std::vector<uint8_t> &out);

  // キャンバス全面の RGBA から CHNKExta のペイロードを組む。
  // `sizes` は Attribute へ書き戻す用。
  bool buildChunkPayload(const uint8_t *rgba, uint32_t w, uint32_t h,
                         const OffscreenAttr &attr,
                         std::vector<uint8_t> &payload,
                         std::vector<uint32_t> &sizes);

  // --- Attribute ----------------------------------------------------------

  // BlockSize 配列だけ差し替える (セクション長は変わらない)。
  bool patchBlockSizes(std::vector<uint8_t> &attr,
                       const std::vector<uint32_t> &sizes);

  // 別の寸法へ作り替える。sizes == nullptr なら全ブロック空 (104)。
  bool retargetAttribute(const uint8_t *attr, int len, uint32_t w, uint32_t h,
                         const std::vector<uint32_t> *sizes,
                         std::vector<uint8_t> &out);

  // ミップ段数と寸法の実測則: 100% から //2 で縮小し、
  // **グリッドが 1x1 になった段の次の段まで**作る。
  std::vector<std::pair<uint32_t, uint32_t>> mipLevels(uint32_t w, uint32_t h);

  // --- PNG (CanvasPreview 用) ---------------------------------------------

  // RGBA8 → PNG。zlib しか使わない最小実装 (フィルタは常に 0)。
  bool encodePng(const uint8_t *rgba, uint32_t w, uint32_t h,
                 std::vector<uint8_t> &png);

} // namespace clip

#endif
