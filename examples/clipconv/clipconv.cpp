// clipconv — CLIP <-> PSD を相互変換する外部サンプル。
//
//   clipconv in.clip out.psd  [--verify] [--flatten]
//   clipconv in.psd  out.clip [--template empty.clip] [--paper] [--verify]
//
// **clipparse と psdparse の両方を参照する側のサンプル**。どちらのライブラリも
// このコマンドのために特別な口を持っていない — 公開 API だけで書いてある。
// Python 版 (tools/clip_to_psd.py, tools/psd_to_clip.py) と同じことをする。
//
// PSD -> CLIP は**空の .clip を雛形にする**。`Layer` 57 列 / `Canvas` 35 列の
// うち CSP が期待する既定値の大半は意味が分かっていないので、ゼロから行を
// 書くより既存行を作り替える方が安全 (docs/DESIGN.md §6.1)。
// 雛形は CSP で「新規」しただけのファイルでよい (既定: samples/emptyimage.clip)。
//
// 制限 (「完全でなくてよい」の範囲):
//   - ラスタとフォルダのみ。テキスト・ベクタ・調整レイヤはラスタ化されて入る
//   - マスクとクリッピングはアルファに焼き込まれる (編集可能な形では持ち込まない)
//   - 覆い焼き(発光) は PSD 側に対応が無く、半透明部が非可逆

#include "clipfile.h"
#include "clipwrite.h"
#include "psdfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- 合成モードの対応表 ----------------------------------------------------
//
// docs/CLIP_FORMAT.md §9 で実測同定した 27 種。PSD 側は 4 文字コード。
// 10 (覆い焼き発光) と 12 (加算発光) は PSD に対応が無いので、
// いちばん近い 9 / 11 へ落とす (12 は等価なので可逆、10 は非可逆)。

struct BlendPair { int64_t clip; int psd; };

static const BlendPair kBlendMap[] = {
  {  0, 'norm' }, {  1, 'dark' }, {  2, 'mul ' }, {  3, 'idiv' }, {  4, 'lbrn' },
  {  5, 'fsub' }, {  6, 'dkCl' }, {  7, 'lite' }, {  8, 'scrn' }, {  9, 'div ' },
  { 10, 'div ' }, { 11, 'lddg' }, { 12, 'lddg' }, { 13, 'lgCl' }, { 14, 'over' },
  { 15, 'sLit' }, { 16, 'hLit' }, { 17, 'vLit' }, { 18, 'lLit' }, { 19, 'pLit' },
  { 20, 'hMix' }, { 21, 'diff' }, { 22, 'smud' }, { 23, 'hue ' }, { 24, 'sat ' },
  { 25, 'colr' }, { 26, 'lum ' }, { 30, 'pass' }, { 36, 'fdiv' },
};

static int clipToPsdBlend(int64_t comp) {
  for (const BlendPair &b : kBlendMap)
    if (b.clip == comp) return b.psd;
  return 'norm';
}

static int64_t psdToClipBlend(int key) {
  // 1 対多だったものは情報を保つ側 (9 / 11) を選ぶ
  for (const BlendPair &b : kBlendMap)
    if (b.psd == key && b.clip != 10 && b.clip != 12) return b.clip;
  return 0;
}

// CLIP の不透明度は 0..256、PSD は 0..255。段数が違うので厳密には 1 対 1 に
// ならないが、**切り捨てと切り上げの組で c=1 以外は往復して戻る** [実測]。
static int clipOpacityToPsd(int64_t opa) {
  return int(std::min<int64_t>(255, opa * 255 / 256));
}
static int64_t psdOpacityToClip(int opa) {
  return std::min<int64_t>(256, (int64_t(opa) * 256 + 254) / 255);
}

// ---- 細かい道具 ------------------------------------------------------------

static std::string u16ToUtf8(const psd::u16str &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
      const uint32_t lo = s[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out.push_back(char(cp));
    } else if (cp < 0x800) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

static std::string psdLayerName(const psd::LayerInfo &l) {
  // `layerName` は旧来のパスカル文字列 (MBCS のことがある)。
  // Unicode 名があればそちらを使う。
  const std::string u = u16ToUtf8(l.layerNameUnicode);
  return u.empty() ? l.layerName : u;
}

static void rgbaToBgra(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
  out.resize(in.size());
  for (size_t i = 0; i < in.size(); i += 4) {
    out[i + 0] = in[i + 2];
    out[i + 1] = in[i + 1];
    out[i + 2] = in[i + 0];
    out[i + 3] = in[i + 3];
  }
}

static bool endsWith(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (tolower(s[s.size() - n + i]) != suffix[i]) return false;
  return true;
}

// ---- CLIP -> PSD -----------------------------------------------------------

struct ClipToPsd {
  const clip::ClipFile &src;
  psd::PSDFile &dst;
  bool useFolders = true;
  int added = 0;

  // CLIP の子チェーンは**先頭が最下層**、PSD の平坦リストも下が先。
  // 順序はそのまま使える。
  int build(const std::vector<int> &kids, int depth) {
    int count = 0;
    for (int i : kids) {
      const clip::LayerInfo &li = src.layers()[size_t(i)];
      const int key = clipToPsdBlend(li.composite);
      const int opa = clipOpacityToPsd(li.opacity);

      if (li.isGroup) {
        const int start = int(dst.layerList.size());
        const int inner = build(li.children, depth + 1);
        if (useFolders) {
          const int idx = dst.addFolder(li.name.c_str(), start, inner,
                                        (li.folder & 16) != 0, key, opa);
          if (idx >= 0) {
            if (!li.visibility) dst.layerList[size_t(idx)].flag |= 2;
            count += inner + 2;          // フォルダ + 区切りの 2 枚組
            printf("    %*sfolder %-16s blend=%lld opa=%d (%d 枚)\n",
                   depth * 2, "", li.name.c_str(), (long long)li.composite,
                   opa, inner);
            ++added;
            continue;
          }
        }
        count += inner;                  // 平坦化した
        continue;
      }
      if (li.bounds.w <= 0 || li.bounds.h <= 0) continue;

      clip::Image img;
      if (!src.layerImage(i, clip::IMAGE_MODE_MASKED, img)) continue;
      std::vector<uint8_t> bgra;
      rgbaToBgra(img.rgba, bgra);
      const int idx = dst.addLayer(li.name.c_str(), li.bounds.x, li.bounds.y,
                                   bgra.data(), int(img.width), int(img.height),
                                   key, opa);
      if (idx < 0) continue;
      if (!li.visibility) dst.layerList[size_t(idx)].flag |= 2;
      if (li.clipping) dst.layerList[size_t(idx)].clipping = 1;
      printf("    %*sadd    %-16s blend=%lld opa=%d %dx%d\n", depth * 2, "",
             li.name.c_str(), (long long)li.composite, opa,
             img.width, img.height);
      ++added;
      ++count;
    }
    return count;
  }
};

static int clipToPsd(const char *in, const char *out, bool useFolders,
                     bool verify) {
  clip::ClipFile f;
  if (!f.load(in)) {
    fprintf(stderr, "cannot read %s: %s\n", in, f.error().c_str());
    return 1;
  }
  const int W = int(f.canvasWidth()), H = int(f.canvasHeight());
  printf("  %s  %dx%d  レイヤ %zu\n", in, W, H, f.layers().size());

  psd::PSDFile psd;
  if (!psd.createBlank(W, H)) {
    fprintf(stderr, "createBlank failed\n");
    return 1;
  }
  ClipToPsd conv{ f, psd, useFolders, 0 };
  conv.build(f.roots(), 0);

  // 完成画も入れておく。**Photoshop で開く前でも見た目が正しくなる**。
  clip::Image merged;
  if (f.mergedImage(merged)) {
    std::vector<uint8_t> bgra;
    rgbaToBgra(merged.rgba, bgra);
    psd.setMergedImage(bgra.data(), W, H);
  }
  if (!psd.save(out)) {
    fprintf(stderr, "cannot write %s\n", out);
    return 1;
  }
  printf("  %s  (%d 層)\n", out, int(psd.layerList.size()));

  if (verify) {
    psd::PSDFile back;
    if (!back.load(out)) {
      fprintf(stderr, "verify: cannot reload %s\n", out);
      return 1;
    }
    int worst = 0, checked = 0;
    for (const psd::LayerInfo &l : back.layerList) {
      if (l.layerType != psd::LAYER_TYPE_NORMAL) continue;
      if (l.width <= 0 || l.height <= 0) continue;
      const std::string name = psdLayerName(l);
      int idx = -1;
      for (size_t k = 0; k < f.layers().size(); ++k)
        if (f.layers()[k].name == name && !f.layers()[k].isGroup) { idx = int(k); break; }
      if (idx < 0) continue;
      clip::Image want;
      if (!f.layerImage(idx, clip::IMAGE_MODE_MASKED, want)) continue;
      if (int(want.width) != l.width || int(want.height) != l.height) continue;
      std::vector<uint8_t> got(size_t(l.width) * l.height * 4);
      back.getLayerImage(l, got.data(), psd::BGRA_LE, l.width * 4,
                         psd::IMAGE_MODE_MASKEDIMAGE);
      for (size_t p = 0; p < got.size(); p += 4) {
        const int a = want.rgba[p + 3];
        const int d[4] = { abs(got[p + 0] - want.rgba[p + 2]),
                           abs(got[p + 1] - want.rgba[p + 1]),
                           abs(got[p + 2] - want.rgba[p + 0]),
                           abs(got[p + 3] - a) };
        // α=0 の RGB は意味を持たない
        for (int c = 0; c < 4; ++c)
          if (c == 3 || a || got[p + 3]) worst = std::max(worst, d[c]);
      }
      ++checked;
    }
    printf("  検証: %d レイヤの画素 max=%d\n", checked, worst);
    return worst == 0 ? 0 : 1;
  }
  return 0;
}

// ---- PSD -> CLIP -----------------------------------------------------------

struct PsdToClip {
  psd::PSDFile &src;
  clip::ClipWriter &dst;
  int64_t templateLayer = 0;
  uint32_t W = 0, H = 0;
  int added = 0;

  // PSD レイヤの画素をキャンバス全面の RGBA へ置く。
  // **CLIP の 100% ミップは常にキャンバス全面**なので、矩形をそこへ貼る。
  void canvasRgba(const psd::LayerInfo &l, std::vector<uint8_t> &out) {
    out.assign(size_t(W) * H * 4, 0);
    if (l.width <= 0 || l.height <= 0) return;
    std::vector<uint8_t> bgra(size_t(l.width) * l.height * 4);
    if (!src.getLayerImage(l, bgra.data(), psd::BGRA_LE, l.width * 4,
                           psd::IMAGE_MODE_MASKEDIMAGE))
      return;
    const int x0 = std::max(0, l.left), y0 = std::max(0, l.top);
    const int x1 = std::min<int>(int(W), l.left + l.width);
    const int y1 = std::min<int>(int(H), l.top + l.height);
    for (int y = y0; y < y1; ++y) {
      const uint8_t *s = &bgra[(size_t(y - l.top) * l.width + (x0 - l.left)) * 4];
      uint8_t *d = &out[(size_t(y) * W + x0) * 4];
      for (int x = x0; x < x1; ++x, s += 4, d += 4) {
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
      }
    }
  }

  bool build(const std::vector<int> &kids, int64_t parent, int depth) {
    for (int i : kids) {
      const psd::LayerInfo &l = src.layerList[size_t(i)];
      if (l.layerType == psd::LAYER_TYPE_HIDDEN) continue;   // 区切りは持ち込まない
      const std::string name = psdLayerName(l);
      const int64_t comp = psdToClipBlend(l.blendModeKey);
      const int64_t opa = psdOpacityToClip(l.opacity);

      if (l.layerType == psd::LAYER_TYPE_FOLDER) {
        const int64_t fid = dst.addLayer(templateLayer, name, nullptr, 0, 0, -1,
                                         parent);
        if (!fid) {
          fprintf(stderr, "addLayer(folder) failed: %s\n", dst.error().c_str());
          return false;
        }
        clip::ClipWriter::LayerAttr a;
        a.opacity = opa;
        a.visibility = l.isVisible() ? 1 : 0;
        a.composite = comp;
        a.folder = 1;                       // bit0 = フォルダ
        dst.setLayerAttr(fid, a);
        setLayerType(fid, 0);               // フォルダは LayerType = 0
        printf("    %*s[F] %-16s comp=%lld opa=%lld\n", depth * 2, "",
               name.c_str(), (long long)comp, (long long)opa);
        ++added;
        if (!build(src.childIndices(i), fid, depth + 1)) return false;
        continue;
      }

      std::vector<uint8_t> rgba;
      canvasRgba(l, rgba);
      const int64_t id = dst.addLayer(templateLayer, name, rgba.data(), W, H, -1,
                                      parent);
      if (!id) {
        fprintf(stderr, "addLayer failed: %s\n", dst.error().c_str());
        return false;
      }
      clip::ClipWriter::LayerAttr a;
      a.opacity = opa;
      a.visibility = l.isVisible() ? 1 : 0;
      a.composite = comp;
      a.clipping = l.clipping ? 1 : 0;
      dst.setLayerAttr(id, a);
      printf("    %*s%-20s comp=%lld opa=%lld\n", depth * 2, "", name.c_str(),
             (long long)comp, (long long)opa);
      ++added;
    }
    return true;
  }

  // `LayerType` は ClipWriter の属性 API に無い (フォルダ専用なので)。
  // 生の SQLite ハンドルで触る — モデル化していない列への逃げ道。
  void setLayerType(int64_t mainId, int64_t type);
};

#include "sqlite3.h"

void PsdToClip::setLayerType(int64_t mainId, int64_t type) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(dst.db(), "UPDATE Layer SET LayerType=? WHERE MainId=?",
                         -1, &st, nullptr) != SQLITE_OK)
    return;
  sqlite3_bind_int64(st, 1, type);
  sqlite3_bind_int64(st, 2, mainId);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

// 書いた後で合成し直して CanvasPreview に入れる。
// **CSP は開いた直後ここを表示する**ので、雛形のままだと最初だけ白い。
static bool refreshPreview(const char *path) {
  clip::Image img;
  {
    clip::ClipFile f;
    if (!f.load(path) || !f.mergedImage(img)) return false;
  }                                     // mmap を手放してから書く
  clip::ClipWriter w;
  if (!w.load(path)) return false;
  if (!w.setCanvasPreview(img.rgba.data(), img.width, img.height)) return false;
  return w.save(path) > 0;
}

static int psdToClip(const char *in, const char *out, const char *tmpl,
                     bool keepPaper, bool verify) {
  psd::PSDFile psd;
  if (!psd.load(in)) {
    fprintf(stderr, "cannot read %s\n", in);
    return 1;
  }
  const uint32_t W = uint32_t(psd.header.width), H = uint32_t(psd.header.height);
  printf("  %s  %ux%u  レイヤ %zu\n", in, W, H, psd.layerList.size());

  // 雛形のラスタレイヤを調べる (複製元として最後まで残す)
  int64_t paperLayer = 0, templateLayer = 0;
  {
    clip::ClipFile t;
    if (!t.load(tmpl)) {
      fprintf(stderr, "cannot read template %s: %s\n", tmpl, t.error().c_str());
      return 1;
    }
    for (int i : t.roots()) {
      const clip::LayerInfo &li = t.layers()[size_t(i)];
      if (li.isGroup) continue;
      if (!paperLayer) paperLayer = li.mainId;
      templateLayer = li.mainId;
    }
    if (paperLayer == templateLayer || !templateLayer) {
      fprintf(stderr, "template needs at least 2 raster layers (paper + one)\n");
      return 1;
    }
  }

  clip::ClipWriter w;
  if (!w.load(tmpl)) {
    fprintf(stderr, "%s\n", w.error().c_str());
    return 1;
  }
  if (!w.resizeCanvas(W, H, psd.header.hres > 0 ? psd.header.hres : 0.0)) {
    fprintf(stderr, "resizeCanvas failed: %s\n", w.error().c_str());
    return 1;
  }

  PsdToClip conv{ psd, w, templateLayer, W, H, 0 };
  if (!conv.build(psd.childIndices(-1), 0, 0)) return 1;

  if (!keepPaper) w.deleteLayer(paperLayer);
  w.deleteLayer(templateLayer);

  const int64_t n = w.save(out);
  if (!n) {
    fprintf(stderr, "%s\n", w.error().c_str());
    return 1;
  }
  if (!refreshPreview(out)) {
    fprintf(stderr, "refreshPreview failed\n");
    return 1;
  }
  printf("  %s  (%d レイヤ, 用紙=%s)\n", out, conv.added, keepPaper ? "あり" : "なし");

  // **CSP で開く前に必ず参照整合性を見る。** ここで落ちる種類の間違いは
  // 寛容なリーダでは読めてしまう。
  std::vector<std::string> problems;
  if (!clip::validate(out, problems)) {
    for (const std::string &p : problems) fprintf(stderr, "  NG %s\n", p.c_str());
    return 1;
  }
  printf("  参照整合性 OK\n");

  if (verify) {
    clip::ClipFile back;
    if (!back.load(out)) {
      fprintf(stderr, "verify: cannot reload\n");
      return 1;
    }
    int worst = 0, checked = 0;
    for (const psd::LayerInfo &l : psd.layerList) {
      if (l.layerType != psd::LAYER_TYPE_NORMAL) continue;
      const std::string name = psdLayerName(l);
      int idx = -1;
      for (size_t k = 0; k < back.layers().size(); ++k)
        if (back.layers()[k].name == name && !back.layers()[k].isGroup) {
          idx = int(k);
          break;
        }
      if (idx < 0) continue;
      std::vector<uint8_t> want;
      conv.canvasRgba(l, want);
      clip::Image got;
      if (!back.layerImage(idx, clip::IMAGE_MODE_MASKED, got)) continue;
      if (got.width != W || got.height != H) continue;
      for (size_t p = 0; p < want.size(); p += 4) {
        const bool vis = want[p + 3] || got.rgba[p + 3];
        for (int c = 0; c < 4; ++c) {
          if (c < 3 && !vis) continue;    // α=0 の RGB は意味を持たない
          worst = std::max(worst, abs(int(got.rgba[p + c]) - int(want[p + c])));
        }
      }
      ++checked;
    }
    printf("  検証: %d レイヤの画素 max=%d\n", checked, worst);
    return worst == 0 ? 0 : 1;
  }
  return 0;
}

// ---- 入口 ------------------------------------------------------------------

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: clipconv in.clip out.psd  [--verify] [--flatten]\n"
            "       clipconv in.psd  out.clip [--template empty.clip]"
            " [--paper] [--verify]\n");
    return 2;
  }
  const std::string in = argv[1], out = argv[2];
  bool verify = false, flatten = false, paper = false;
  const char *tmpl = "samples/emptyimage.clip";
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--verify") == 0) verify = true;
    else if (strcmp(argv[i], "--flatten") == 0) flatten = true;
    else if (strcmp(argv[i], "--paper") == 0) paper = true;
    else if (strcmp(argv[i], "--template") == 0 && i + 1 < argc) tmpl = argv[++i];
    else {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }
  }

  if (endsWith(in, ".clip") && endsWith(out, ".psd"))
    return clipToPsd(in.c_str(), out.c_str(), !flatten, verify);
  if (endsWith(in, ".psd") && endsWith(out, ".clip"))
    return psdToClip(in.c_str(), out.c_str(), tmpl, paper, verify);

  fprintf(stderr, "cannot tell the direction from the file names\n");
  return 2;
}
