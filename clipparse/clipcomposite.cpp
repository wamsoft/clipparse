// レイヤ画像の取得と合成。
//
// 式はすべて実測で同定したもの (docs/CLIP_FORMAT.md §9 / §10)。
// Python の参照実装 (tools/clip_lazy_demo.py) と**画素バイト単位で一致する**
// ことを回帰で確かめている。片方を直したらもう片方も直すこと。

#include "clipfile.h"
#include "sqlite3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace clip {

  namespace {

    const int PASS_THROUGH = 30;          // LayerComposite。フォルダ専用

    inline bool isGlow(int mode) {        // 覆い焼き(発光) / 加算(発光)
      return mode == 10 || mode == 12;
    }

    inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    // 0..255 へ丸める。**numpy の np.round と同じ「偶数丸め」**にしてある。
    // Python の参照実装と画素バイト単位で一致させるため (floor(x+0.5) だと
    // ちょうど .5 の画素だけ 1 ずれる)。
    inline uint8_t to8(double v) {
      if (v <= 0.0) return 0;
      if (v >= 255.0) return 255;
      return uint8_t(std::nearbyint(v));
    }

    inline double luma(const double c[3]) {
      return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
    }
    inline double satOf(const double c[3]) {
      return std::max(c[0], std::max(c[1], c[2])) -
             std::min(c[0], std::min(c[1], c[2]));
    }
    void setLuma(double c[3], double l) {
      const double d = l - luma(c);
      c[0] += d; c[1] += d; c[2] += d;
      const double lv = luma(c);
      const double lo = std::min(c[0], std::min(c[1], c[2]));
      const double hi = std::max(c[0], std::max(c[1], c[2]));
      if (lo < 0.0)
        for (int i = 0; i < 3; i++) c[i] = lv + (c[i] - lv) * lv / std::max(lv - lo, 1e-9);
      if (hi > 1.0)
        for (int i = 0; i < 3; i++)
          c[i] = lv + (c[i] - lv) * (1.0 - lv) / std::max(hi - lv, 1e-9);
    }
    void setSat(double c[3], double s) {
      const double mn = std::min(c[0], std::min(c[1], c[2]));
      const double mx = std::max(c[0], std::max(c[1], c[2]));
      if (mx > mn) {
        const double rng = mx - mn;
        for (int i = 0; i < 3; i++) c[i] = (c[i] - mn) * s / rng;
      } else {
        c[0] = c[1] = c[2] = 0.0;
      }
    }

    inline double hardLight(double b, double s) {
      return s <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    }
    inline double softLight(double b, double s) {
      const double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(std::max(b, 0.0));
      return s <= 0.5 ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * (d - b);
    }
    inline double colorDodge(double b, double s) {
      return s >= 1.0 ? 1.0 : std::min(1.0, b / std::max(1 - s, 1e-9));
    }
    inline double colorBurn(double b, double s) {
      return s <= 0.0 ? 0.0 : 1 - std::min(1.0, (1 - b) / std::max(s, 1e-9));
    }

    // 分離可能な合成式。非分離 (色相/彩度/カラー/輝度/カラー比較) は別扱い。
    bool separableBlend(int mode, double b, double s, double &out) {
      switch (mode) {
      case 1:  out = std::min(b, s); return true;                    // 比較 (暗)
      case 2:  out = b * s; return true;                             // 乗算
      case 3:  out = colorBurn(b, s); return true;                   // 焼きこみカラー
      case 4:  out = b + s - 1; return true;                         // 焼きこみ (リニア)
      case 5:  out = b - s; return true;                             // 減算
      case 7:  out = std::max(b, s); return true;                    // 比較 (明)
      case 8:  out = b + s - b * s; return true;                     // スクリーン
      case 9:
      case 10: out = colorDodge(b, s); return true;                  // 覆い焼き
      case 11:
      case 12: out = b + s; return true;                             // 加算
      case 14: out = hardLight(s, b); return true;                   // オーバーレイ
      case 15: out = softLight(b, s); return true;                   // ソフトライト
      case 16: out = hardLight(b, s); return true;                   // ハードライト
      case 17: out = s <= 0.5 ? colorBurn(b, 2 * s)                  // ビビッドライト
                              : colorDodge(b, 2 * (s - 0.5)); return true;
      case 18: out = b + 2 * s - 1; return true;                     // リニアライト
      case 19: out = s <= 0.5 ? std::min(b, 2 * s)                   // ピンライト
                              : std::max(b, 2 * s - 1); return true;
      case 20: out = (b + s >= 1.0) ? 1.0 : 0.0; return true;        // ハードミックス
      case 21: out = std::fabs(b - s); return true;                  // 差の絶対値
      case 22: out = b + s - 2 * b * s; return true;                 // 除外
      case 36: out = b / std::max(s, 1e-9); return true;             // 除算
      default: return false;
      }
    }

    // 画素 1 つ分の合成式。b/s は 0..1 の RGB。
    void blendPixel(int mode, const double b[3], const double s[3], double out[3]) {
      double v;
      if (separableBlend(mode, b[0], s[0], v)) {
        for (int i = 0; i < 3; i++) {
          separableBlend(mode, b[i], s[i], v);
          out[i] = clamp01(v);
        }
        return;
      }
      double t[3] = { s[0], s[1], s[2] };
      switch (mode) {
      case 6: {                                          // カラー比較 (暗)
        const bool takeSrc = luma(s) < luma(b);
        for (int i = 0; i < 3; i++) out[i] = takeSrc ? s[i] : b[i];
        return;
      }
      case 13: {                                         // カラー比較 (明)
        const bool takeSrc = luma(s) > luma(b);
        for (int i = 0; i < 3; i++) out[i] = takeSrc ? s[i] : b[i];
        return;
      }
      case 23:                                           // 色相
        setSat(t, satOf(b)); setLuma(t, luma(b));
        break;
      case 24: {                                         // 彩度
        double u[3] = { b[0], b[1], b[2] };
        setSat(u, satOf(s)); setLuma(u, luma(b));
        for (int i = 0; i < 3; i++) out[i] = clamp01(u[i]);
        return;
      }
      case 25:                                           // カラー
        setLuma(t, luma(b));
        break;
      case 26: {                                         // 輝度
        double u[3] = { b[0], b[1], b[2] };
        setLuma(u, luma(s));
        for (int i = 0; i < 3; i++) out[i] = clamp01(u[i]);
        return;
      }
      default:                                           // 通常 / 未対応
        for (int i = 0; i < 3; i++) out[i] = s[i];
        return;
      }
      for (int i = 0; i < 3; i++) out[i] = clamp01(t[i]);
    }

    // 一般形の合成 (下地が透明でも正しい)。
    //   αo    = αs + αb*(1-αs)
    //   Co*αo = (1-αb)*αs*Cs + αb*αs*B(Cb,Cs) + (1-αs)*αb*Cb
    // 「発光」モードだけは α による補間をせず、s を α で乗じて直接ブレンドする。
    void blendOver(Image &dst, const Image &src, int mode) {
      const size_t n = size_t(dst.width) * dst.height;
      for (size_t i = 0; i < n; i++) {
        uint8_t *d = &dst.rgba[i * 4];
        const uint8_t *s8 = &src.rgba[i * 4];
        const double as = s8[3] / 255.0;
        const double ab = d[3] / 255.0;
        if (as <= 0.0 && !isGlow(mode)) continue;
        double b[3] = { d[0] / 255.0, d[1] / 255.0, d[2] / 255.0 };
        double s[3] = { s8[0] / 255.0, s8[1] / 255.0, s8[2] / 255.0 };
        const double ao = as + ab * (1 - as);
        double outc[3];
        if (isGlow(mode)) {
          double sp[3] = { s[0] * as, s[1] * as, s[2] * as };
          double g[3];
          blendPixel(mode, b, sp, g);
          for (int c = 0; c < 3; c++) {
            const double num = (1 - ab) * as * s[c] + ab * g[c];
            outc[c] = ao > 0 ? num / std::max(ao, 1e-9) : 0.0;
          }
        } else {
          double g[3];
          blendPixel(mode, b, s, g);
          for (int c = 0; c < 3; c++) {
            const double num = (1 - ab) * as * s[c] + ab * as * g[c] + (1 - as) * ab * b[c];
            outc[c] = ao > 0 ? num / std::max(ao, 1e-9) : 0.0;
          }
        }
        for (int c = 0; c < 3; c++) d[c] = to8(outc[c] * 255.0);
        d[3] = to8(ao * 255.0);
      }
    }

    // --- 調整レイヤ ---------------------------------------------------------

    void rgbToHsv(double r, double g, double b, double &h, double &s, double &v) {
      const double mx = std::max(r, std::max(g, b));
      const double mn = std::min(r, std::min(g, b));
      const double d = mx - mn;
      h = 0.0;
      if (d > 0) {
        if (mx == r)      h = std::fmod((g - b) / d, 6.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else              h = (r - g) / d + 4.0;
      }
      h *= 60.0;
      s = mx > 0 ? d / mx : 0.0;
      v = mx;
    }

    void hsvToRgb(double h, double s, double v, double &r, double &g, double &b) {
      h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 60.0;
      const int i = int(std::floor(h));
      const double f = h - i;
      const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
      switch (i % 6) {
      case 0: r = v; g = t; b = p; break;
      case 1: r = q; g = v; b = p; break;
      case 2: r = p; g = v; b = t; break;
      case 3: r = p; g = q; b = v; break;
      case 4: r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
      }
    }

    // トーンカーブの LUT。格納された点は**曲線が通る点ではなくベジェの制御点**。
    bool toneCurveLut(const uint8_t *blob, int len, double lut[256]) {
      if (len < 8) return false;
      const uint32_t payload = beU32(blob + 4);
      if (int(8 + payload) > len || payload < 2) return false;
      const uint8_t *u = blob + 8;
      const uint32_t count = beU16(u);
      if (count < 2 || 2u + count * 4u > payload) return false;
      std::vector<double> px(count), py(count);
      for (uint32_t i = 0; i < count; i++) {
        px[i] = beU16(u + 2 + i * 4) / 65535.0 * 255.0;
        py[i] = beU16(u + 4 + i * 4) / 65535.0 * 255.0;
      }
      // Bernstein 基底で次数 count-1 のベジェを引き、x で並べ替えて LUT にする
      const int N = 20001;
      std::vector<double> bx(N), by(N);
      const int deg = int(count) - 1;
      std::vector<double> comb(count, 1.0);
      for (int i = 1; i <= deg; i++) comb[i] = comb[i - 1] * (deg - i + 1) / i;
      for (int k = 0; k < N; k++) {
        const double t = double(k) / (N - 1);
        double sx = 0, sy = 0;
        for (uint32_t i = 0; i <= uint32_t(deg); i++) {
          const double w = comb[i] * std::pow(1 - t, deg - int(i)) * std::pow(t, int(i));
          sx += w * px[i]; sy += w * py[i];
        }
        bx[k] = sx; by[k] = sy;
      }
      int k = 0;
      for (int v = 0; v < 256; v++) {
        while (k < N - 2 && bx[k + 1] < v) k++;
        const double x0 = bx[k], x1 = bx[k + 1];
        const double f = (x1 > x0) ? (v - x0) / (x1 - x0) : 0.0;
        lut[v] = by[k] + (by[k + 1] - by[k]) * std::min(1.0, std::max(0.0, f));
      }
      return true;
    }

  } // namespace

  // ---- レイヤ 1 枚の画素 ---------------------------------------------------

  bool ClipFile::layerPixels(const LayerInfo &li, Image &out) const {
    const int64_t off = topOffscreen(li.mainId);
    if (!off) return false;
    const OffscreenAttr *a = attribute(off);
    if (!a) return false;

    if (!hasPixels(off) && !a->hasInitColor) {
      // テキスト等: 外接矩形のラスタを配置位置に置く
      const int64_t obj = objectOffscreen(li.mainId);
      if (obj && !li.bounds.empty()) {
        Image o;
        if (readOffscreen(obj, o)) { out = o; return true; }
      }
      // グループ: サムネイルキャッシュしか無いことがある。拡大して使う。
      int64_t thumb = 0;
      sqlite3_stmt *st = nullptr;
      if (li.renderThumbnail &&
          sqlite3_prepare_v2(db_, "SELECT ThumbnailOffscreen FROM LayerThumbnail"
                                  " WHERE MainId=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, li.renderThumbnail);
        if (sqlite3_step(st) == SQLITE_ROW) thumb = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
      }
      if (thumb && hasPixels(thumb)) {
        Image t;
        if (readOffscreen(thumb, t)) {
          out.resize(uint32_t(li.bounds.w), uint32_t(li.bounds.h));
          for (uint32_t y = 0; y < out.height; y++) {
            const uint32_t sy = std::min(t.height - 1, y * t.height / out.height);
            for (uint32_t x = 0; x < out.width; x++) {
              const uint32_t sx = std::min(t.width - 1, x * t.width / out.width);
              memcpy(out.at(x, y), t.at(sx, sy), 4);
            }
          }
          return true;
        }
      }
      return false;
    }
    return readOffscreen(off, out);
  }

  bool ClipFile::layerImage(int index, ImageMode mode, Image &out) const {
    if (index < 0 || index >= int(layers_.size())) return false;
    const LayerInfo &li = layers_[(size_t)index];
    if (li.bounds.empty()) { out.resize(0, 0); return true; }

    const int64_t moff = li.hasMask ? topOffscreen(li.mainId, true) : 0;
    const bool maskOk = moff && hasPixels(moff);

    if (mode == IMAGE_MODE_MASK) {
      out.resize(uint32_t(li.bounds.w), uint32_t(li.bounds.h));
      if (!maskOk) return true;
      Image m;
      if (!readOffscreen(moff, m)) return true;
      for (uint32_t y = 0; y < out.height && y < m.height; y++)
        for (uint32_t x = 0; x < out.width && x < m.width; x++) {
          uint8_t *d = out.at(x, y);
          d[0] = d[1] = d[2] = m.at(x, y)[3];
          d[3] = 255;
        }
      return true;
    }

    Image img;
    if (!layerPixels(li, img)) { out.resize(uint32_t(li.bounds.w), uint32_t(li.bounds.h)); return true; }
    out.resize(uint32_t(li.bounds.w), uint32_t(li.bounds.h));
    for (uint32_t y = 0; y < out.height && y < img.height; y++)
      memcpy(out.at(0, y), img.at(0, y),
             size_t(std::min(out.width, img.width)) * 4);

    if (mode == IMAGE_MODE_MASKED && maskOk) {
      Image m;
      if (readOffscreen(moff, m)) {
        for (uint32_t y = 0; y < out.height && y < m.height; y++)
          for (uint32_t x = 0; x < out.width && x < m.width; x++) {
            uint8_t *d = out.at(x, y);
            d[3] = uint8_t(uint32_t(d[3]) * m.at(x, y)[3] / 255);
          }
      }
    }
    return true;
  }

  bool ClipFile::layerRegion(int index, const Rect &r, ImageMode mode, Image &out) const {
    if (index < 0 || index >= int(layers_.size())) return false;
    const LayerInfo &li = layers_[(size_t)index];
    const int64_t off = topOffscreen(li.mainId);
    if (!off || r.empty()) { out.resize(0, 0); return true; }
    if (!readOffscreenRegion(off, r, out)) return false;
    if (mode == IMAGE_MODE_MASKED && li.hasMask) {
      const int64_t moff = topOffscreen(li.mainId, true);
      if (moff && hasPixels(moff)) {
        Image m;
        if (readOffscreenRegion(moff, r, m)) {
          for (uint32_t y = 0; y < out.height && y < m.height; y++)
            for (uint32_t x = 0; x < out.width && x < m.width; x++) {
              uint8_t *d = out.at(x, y);
              d[3] = uint8_t(uint32_t(d[3]) * m.at(x, y)[3] / 255);
            }
        }
      }
    }
    return true;
  }

  // ---- 合成 ---------------------------------------------------------------

  bool ClipFile::compositeInto(int64_t parentMainId, Image &dst) const {
    const int pidx = layerIndex(parentMainId);
    const std::vector<int> &kids =
      (parentMainId == rootLayer_) ? rootChildren_
                                   : layers_[(size_t)pidx].children;
    std::vector<uint8_t> clipBase;          // 直近の非クリップレイヤの α
    bool haveClipBase = false;

    for (int idx : kids) {
      const LayerInfo &li = layers_[(size_t)idx];
      if (!li.visibility) continue;

      if (li.isGroup && li.composite == PASS_THROUGH) {
        // 通過フォルダ: 分離せず、この階層のバッファへ直接描く
        compositeInto(li.mainId, dst);
        haveClipBase = false;
        continue;
      }

      if (li.isFilter) {
        // 調整レイヤ: 画素を持たず、下にある結果を書き換える
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT FilterLayerInfo FROM Layer WHERE MainId=?",
                               -1, &st, nullptr) == SQLITE_OK) {
          sqlite3_bind_int64(st, 1, li.mainId);
          if (sqlite3_step(st) == SQLITE_ROW) {
            const uint8_t *b = (const uint8_t *)sqlite3_column_blob(st, 0);
            const int n = sqlite3_column_bytes(st, 0);
            if (b && n >= 8) applyFilter(dst, b, n);
          }
          sqlite3_finalize(st);
        }
        continue;
      }

      Image img;
      if (li.isGroup) {
        img.resize(dst.width, dst.height);
        compositeInto(li.mainId, img);
      } else {
        Image raw;
        if (!layerPixels(li, raw)) continue;
        img.resize(dst.width, dst.height);
        // bounds の位置に置く (テキストは外接矩形)
        for (uint32_t y = 0; y < raw.height; y++) {
          const int dy = li.bounds.y + int(y);
          if (dy < 0 || dy >= int(dst.height)) continue;
          for (uint32_t x = 0; x < raw.width; x++) {
            const int dx = li.bounds.x + int(x);
            if (dx < 0 || dx >= int(dst.width)) continue;
            memcpy(img.at(uint32_t(dx), uint32_t(dy)), raw.at(x, y), 4);
          }
        }
        // レイヤマスク
        if (li.hasMask) {
          const int64_t moff = topOffscreen(li.mainId, true);
          if (moff && hasPixels(moff)) {
            Image m;
            if (readOffscreen(moff, m)) {
              for (uint32_t y = 0; y < img.height && y < m.height; y++)
                for (uint32_t x = 0; x < img.width && x < m.width; x++) {
                  uint8_t *d = img.at(x, y);
                  d[3] = uint8_t(uint32_t(d[3]) * m.at(x, y)[3] / 255);
                }
            }
          }
        }
      }

      // 不透明度 (0..256 なので 256 で割る)
      if (li.opacity < 256) {
        for (size_t i = 3; i < img.rgba.size(); i += 4)
          img.rgba[i] = uint8_t(uint32_t(img.rgba[i]) * uint32_t(li.opacity) / 256);
      }

      // クリッピング: 直下の非クリップレイヤの α で絞る
      if (li.clipping) {
        if (haveClipBase) {
          for (size_t i = 0; i < clipBase.size(); i++)
            img.rgba[i * 4 + 3] =
              uint8_t((uint32_t(img.rgba[i * 4 + 3]) * clipBase[i] + 127) / 255);
        }
      } else {
        clipBase.resize(size_t(img.width) * img.height);
        for (size_t i = 0; i < clipBase.size(); i++) clipBase[i] = img.rgba[i * 4 + 3];
        haveClipBase = true;
      }

      blendOver(dst, img, int(li.composite));
    }
    return true;
  }

  void ClipFile::applyFilter(Image &dst, const uint8_t *blob, int len) const {
    const uint32_t ftype = beU32(blob);
    const uint32_t nbytes = beU32(blob + 4);
    const int nparam = int(nbytes / 4);
    std::vector<int32_t> p{};
    p.resize(size_t(nparam < 0 ? 0 : nparam));
    for (int i = 0; i < nparam && 8 + 4 * i + 4 <= len; i++)
      p[(size_t)i] = int32_t(beU32(blob + 8 + 4 * i));

    double lut[256];
    const bool haveLut = (ftype == 3) && toneCurveLut(blob, len, lut);
    if (ftype == 3 && !haveLut) return;

    const size_t n = size_t(dst.width) * dst.height;
    for (size_t i = 0; i < n; i++) {
      uint8_t *d = &dst.rgba[i * 4];
      double v[3] = { double(d[0]), double(d[1]), double(d[2]) };
      switch (ftype) {
      case 1:                                     // 明るさ・コントラスト
        if (nparam >= 1) for (int c = 0; c < 3; c++) v[c] += p[0];
        break;
      case 3:                                     // トーンカーブ
        for (int c = 0; c < 3; c++) v[c] = lut[int(std::min(255.0, std::max(0.0, v[c])))];
        break;
      case 4: {                                   // 色相・彩度・明度
        if (nparam < 3) break;                    // 彩度/明度の式は未確定 (色相のみ)
        double h, s, val, r, g, b;
        rgbToHsv(v[0] / 255.0, v[1] / 255.0, v[2] / 255.0, h, s, val);
        hsvToRgb(h + p[0], s, val, r, g, b);
        v[0] = r * 255.0; v[1] = g * 255.0; v[2] = b * 255.0;
        break;
      }
      case 6:                                     // 階調の反転
        for (int c = 0; c < 3; c++) v[c] = 255.0 - v[c];
        break;
      case 8:                                     // 2値化 (チャンネルごと)
        if (nparam >= 1)
          for (int c = 0; c < 3; c++) v[c] = (v[c] >= p[0]) ? 255.0 : 0.0;
        break;
      default:
        return;                                   // 未対応の種別は素通し
      }
      for (int c = 0; c < 3; c++) d[c] = to8(v[c]);
    }
  }

  bool ClipFile::mergedImage(Image &out) const {
    out.resize(uint32_t(canvasPixelW_), uint32_t(canvasPixelH_));
    return compositeInto(rootLayer_, out);
  }

  bool ClipFile::previewPng(std::vector<uint8_t> &png, int &w, int &h) const {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT ImageWidth, ImageHeight, ImageData"
                                " FROM CanvasPreview LIMIT 1",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
      w = sqlite3_column_int(st, 0);
      h = sqlite3_column_int(st, 1);
      const uint8_t *b = (const uint8_t *)sqlite3_column_blob(st, 2);
      const int n = sqlite3_column_bytes(st, 2);
      if (b && n > 0) { png.assign(b, b + n); ok = true; }
    }
    sqlite3_finalize(st);
    return ok;
  }

} // namespace clip
