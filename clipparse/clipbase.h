#ifndef __clipbase_h__
#define __clipbase_h__

// clipparse の土台。psdparse の psdbase.h と同じ思想で、
// 「バイトをどこから供給するか」を抽象化する。
//
// CLIP は PSD と違い、SQLite 側に全ブロックの絶対オフセットがあるので、
// パーサが逐次読みする必要がない。よってイテレータではなく
// **ランダムアクセス可能なバイト供給** (readAt) を基本インタフェースにする。

#include <cstdint>
#include <cstddef>
#include <cstring>      // memcpy。MSVC / Apple clang は間接的に入るが GCC は入らない
#include <string>
#include <vector>

namespace clip {

  // ---- エンディアン (CLIP は特記なき限りビッグエンディアン) ----------------

  inline uint16_t beU16(const uint8_t *p) {
    return uint16_t(p[0]) << 8 | uint16_t(p[1]);
  }
  inline uint32_t beU32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
           uint32_t(p[2]) << 8  | uint32_t(p[3]);
  }
  inline uint64_t beU64(const uint8_t *p) {
    return uint64_t(beU32(p)) << 32 | uint64_t(beU32(p + 4));
  }
  // 例外: ブロックの zlib 圧縮長だけリトルエンディアン
  inline uint32_t leU32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }

  // ---- バイト供給の抽象 ---------------------------------------------------
  //
  // mmap 実装 (MemorySource) では data() が非 null を返し、呼び出し側は
  // コピーなしで直接参照できる。ストリーム実装では data() が null を返し、
  // readAt() でコピーを受け取る。

  class Source {
  public:
    virtual ~Source() {}

    // 全体サイズ
    virtual int64_t size() const = 0;

    // [offset, offset+len) を buf へ読む。読めたバイト数を返す。
    virtual int64_t readAt(int64_t offset, void *buf, int64_t len) const = 0;

    // 連続メモリ上にあるなら先頭ポインタ。無ければ nullptr。
    virtual const uint8_t *data() const { return nullptr; }

    // ヘルパ: 連続メモリならゼロコピー、そうでなければ tmp へ読んでから返す
    const uint8_t *peek(int64_t offset, int64_t len, std::vector<uint8_t> &tmp) const {
      if (const uint8_t *p = data()) {
        if (offset < 0 || offset + len > size()) return nullptr;
        return p + offset;
      }
      tmp.resize(size_t(len));
      if (readAt(offset, tmp.data(), len) != len) return nullptr;
      return tmp.data();
    }
  };

  // 連続メモリ (mmap したファイル / 呼び出し元のバッファ) を指す Source。
  class MemorySource : public Source {
  public:
    MemorySource(const uint8_t *base, int64_t len) : base_(base), len_(len) {}
    int64_t size() const override { return len_; }
    const uint8_t *data() const override { return base_; }
    int64_t readAt(int64_t offset, void *buf, int64_t len) const override {
      if (offset < 0 || offset + len > len_) return 0;
      memcpy(buf, base_ + offset, size_t(len));
      return len;
    }
  private:
    const uint8_t *base_;
    int64_t len_;
  };

  // UTF-16BE (CLIP のマーカー文字列) → UTF-8。ASCII 範囲しか出てこない前提。
  inline std::string utf16beToAscii(const uint8_t *p, int chars) {
    std::string s;
    s.reserve(size_t(chars));
    for (int i = 0; i < chars; ++i) s.push_back(char(p[i * 2 + 1]));
    return s;
  }

} // namespace clip

#endif
