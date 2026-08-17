// clipparse のスモークテスト CLI。
//
//   clip_cli file.clip                          構造をダンプ
//   clip_cli file.clip --check                  全ブロックの構造アサーション
//   clip_cli file.clip --merged out.raw         合成結果を RGBA8 で書き出す
//   clip_cli file.clip --layer N out.raw        レイヤ N を RGBA8 で書き出す
//   clip_cli file.clip --region N x y w h [out] レイヤ N の一部だけ読む
//   clip_cli file.clip --dump-offscreen ID out.raw
//
// 書く側 (tools/clip_write.py と同じことをする):
//
//   clip_cli in.clip --validate                    参照整合性を検査する
//   clip_cli in.clip --roundtrip out.clip          無変更で読み書きする
//   clip_cli in.clip --set N --opacity V --out out.clip
//   clip_cli in.clip --set-pixels N rgba.raw out.clip
//   clip_cli in.clip --add-layer  N rgba.raw out.clip [--name S]
//
// rgba.raw は**キャンバス全面の RGBA8 生バイト** (Python 側と受け渡すため)。
// `-` を渡すと透明レイヤになる。
//
// Python 版 (tools/) と同じ結果になることを確認するために使う。

#include "clipfile.h"
#include "clipwrite.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool writeRaw(const char *path, const std::vector<uint8_t> &data) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "cannot write %s\n", path);
    return false;
  }
  fwrite(data.data(), 1, data.size(), fp);
  fclose(fp);
  return true;
}

static bool readRaw(const char *path, std::vector<uint8_t> &out) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "cannot read %s\n", path);
    return false;
  }
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  out.resize(size_t(n));
  const bool ok = fread(out.data(), 1, size_t(n), fp) == size_t(n);
  fclose(fp);
  return ok;
}

static void dumpTree(const clip::ClipFile &f, int idx, int depth) {
  const clip::LayerInfo &li = f.layers()[size_t(idx)];
  const char *kind = li.isGroup  ? "FOLDER"
                   : li.isFilter ? "ADJUST"
                   : li.isText   ? "TEXT  " : "NORMAL";
  printf("%*s[%d] %-20s %s comp=%-3lld opa=%lld/256 rect=(%d,%d %dx%d)%s%s%s\n",
         depth * 2, "", idx, li.name.c_str(), kind,
         (long long)li.composite, (long long)li.opacity,
         li.bounds.x, li.bounds.y, li.bounds.w, li.bounds.h,
         li.visibility ? "" : " hidden",
         li.hasMask ? " mask" : "",
         li.clipping ? " clip" : "");
  for (int c : li.children) dumpTree(f, c, depth + 1);
}

// 書いた後で合成し直して CanvasPreview に入れる。
// **CSP は開いた直後ここを表示する**ので、古いままだと最初だけ違う絵が出る。
static bool refreshPreview(const char *path) {
  clip::Image img;
  {
    clip::ClipFile f;
    if (!f.load(path)) return false;
    if (!f.mergedImage(img)) return false;
  }                                     // mmap を手放してから書く
  clip::ClipWriter w;
  if (!w.load(path)) return false;
  if (!w.setCanvasPreview(img.rgba.data(), img.width, img.height)) return false;
  return w.save(path) > 0;
}

static uint64_t idSeedOf(int argc, char **argv, bool *seeded) {
  *seeded = false;
  for (int j = 2; j + 1 < argc; ++j)
    if (strcmp(argv[j], "--id-seed") == 0) {
      *seeded = true;
      return strtoull(argv[j + 1], nullptr, 10);
    }
  return 0;
}

// 書く側のサブコマンド。処理したら *handled を立てる。
static int writeCommands(int argc, char **argv, bool *handled) {
  *handled = true;
  const char *src = argv[1];

  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--validate") == 0) {
      std::vector<std::string> problems;
      const bool ok = clip::validate(src, problems);
      for (const std::string &p : problems) printf("  NG %s\n", p.c_str());
      printf("  => %s\n", ok ? "OK" : "NG");
      return ok ? 0 : 1;
    }

    if (strcmp(argv[i], "--roundtrip") == 0 && i + 1 < argc) {
      clip::ClipWriter w;
      if (!w.load(src)) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      const int64_t n = w.save(argv[i + 1]);
      if (!n) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      printf("  %s -> %s (%lld B)\n", src, argv[i + 1], (long long)n);
      return 0;
    }

    if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
      clip::ClipWriter w;
      if (!w.load(src)) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      clip::ClipWriter::LayerAttr a;
      const char *dst = nullptr;
      const long long layer = atoll(argv[i + 1]);
      for (int j = i + 2; j + 1 < argc; ++j) {
        if (strcmp(argv[j], "--opacity") == 0)        a.opacity = atoll(argv[++j]);
        else if (strcmp(argv[j], "--visible") == 0)   a.visibility = atoll(argv[++j]);
        else if (strcmp(argv[j], "--composite") == 0) a.composite = atoll(argv[++j]);
        else if (strcmp(argv[j], "--clip") == 0)      a.clipping = atoll(argv[++j]);
        else if (strcmp(argv[j], "--name") == 0)      a.name = argv[++j];
        else if (strcmp(argv[j], "--out") == 0)       dst = argv[++j];
      }
      if (!dst) {
        fprintf(stderr, "--set needs --out\n");
        return 2;
      }
      if (!w.setLayerAttr(layer, a) || !w.save(dst)) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      printf("  layer #%lld -> %s\n", layer, dst);
      return 0;
    }

    const bool add = strcmp(argv[i], "--add-layer") == 0;
    if ((add || strcmp(argv[i], "--set-pixels") == 0) && i + 3 < argc) {
      const long long layer = atoll(argv[i + 1]);
      const char *rawPath = argv[i + 2];
      const char *dst = argv[i + 3];
      const char *name = "layer";
      bool preview = true;
      for (int j = i + 4; j < argc; ++j) {
        if (strcmp(argv[j], "--name") == 0 && j + 1 < argc) name = argv[++j];
        else if (strcmp(argv[j], "--no-preview") == 0) preview = false;
      }
      bool seeded = false;
      const uint64_t seed = idSeedOf(argc, argv, &seeded);

      uint32_t W = 0, H = 0;
      {
        clip::ClipFile probe;
        if (!probe.load(src)) {
          fprintf(stderr, "load failed: %s\n", probe.error().c_str());
          return 1;
        }
        W = uint32_t(probe.canvasWidth());
        H = uint32_t(probe.canvasHeight());
      }

      std::vector<uint8_t> rgba;
      const bool empty = strcmp(rawPath, "-") == 0;
      if (!empty) {
        if (!readRaw(rawPath, rgba)) return 1;
        if (rgba.size() != size_t(W) * H * 4) {
          fprintf(stderr, "raw size %zu != %u x %u x 4\n", rgba.size(), W, H);
          return 1;
        }
      }

      clip::ClipWriter w;
      if (seeded) w.setExternalIdSeed(seed);
      if (!w.load(src)) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      if (add) {
        const int64_t id = w.addLayer(layer, name, empty ? nullptr : rgba.data(), W, H);
        if (!id) {
          fprintf(stderr, "%s\n", w.error().c_str());
          return 1;
        }
        printf("  new layer #%lld '%s'\n", (long long)id, name);
      } else if (!w.setPixels(layer, rgba.data(), W, H)) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      const int64_t n = w.save(dst);
      if (!n) {
        fprintf(stderr, "%s\n", w.error().c_str());
        return 1;
      }
      if (preview && !refreshPreview(dst)) {
        fprintf(stderr, "refreshPreview failed\n");
        return 1;
      }
      printf("  %s (%lld B)\n", dst, (long long)n);
      return 0;
    }
  }
  *handled = false;
  return 0;
}

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
// Windows の `main` の argv は**アクティブコードページ**で来るので、
// 日本語のレイヤ名がそのままでは化ける。コマンドラインを取り直して
// UTF-8 に直す (ファイル名も clipparse は UTF-8 で受ける)。
static std::vector<std::string> utf8Args(int &argc) {
  std::vector<std::string> out;
  int n = 0;
  LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
  if (!w) return out;
  for (int i = 0; i < n; ++i) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
    out.push_back(s);
  }
  LocalFree(w);
  argc = n;
  return out;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
  std::vector<std::string> wide = utf8Args(argc);
  std::vector<char *> ptrs;
  if (!wide.empty()) {
    for (std::string &s : wide) ptrs.push_back(&s[0]);
    argv = ptrs.data();
  }
#endif
  if (argc < 2) {
    fprintf(stderr,
            "usage: clip_cli file.clip [--check] [--merged out.raw]\n"
            "                         [--layer N out.raw]\n"
            "                         [--region N x y w h [out.raw]]\n"
            "                         [--dump-offscreen ID out.raw]\n"
            "       clip_cli in.clip --validate\n"
            "       clip_cli in.clip --roundtrip out.clip\n"
            "       clip_cli in.clip --set N --opacity V --out out.clip\n"
            "       clip_cli in.clip --set-pixels N rgba.raw out.clip\n"
            "       clip_cli in.clip --add-layer  N rgba.raw out.clip [--name S]\n");
    return 2;
  }

  {
    bool handled = false;
    const int rc = writeCommands(argc, argv, &handled);
    if (handled) return rc;
  }

  clip::ClipFile f;
  if (!f.load(argv[1])) {
    fprintf(stderr, "load failed: %s\n", f.error().c_str());
    return 1;
  }
  printf("%s\n", argv[1]);
  printf("  canvas %lld x %lld @ %.0f dpi  root=#%lld  layers=%zu\n",
         (long long)f.canvasWidth(), (long long)f.canvasHeight(),
         f.canvasResolution(), (long long)f.rootLayer(), f.layers().size());

  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--check") == 0) {
      std::string report;
      const bool ok = f.checkAll(&report);
      printf("--- check ---\n%s", report.c_str());
      printf("  => %s\n", ok ? "OK" : "NG");
      return ok ? 0 : 1;
    }
    if (strcmp(argv[i], "--merged") == 0 && i + 1 < argc) {
      clip::Image img;
      if (!f.mergedImage(img)) {
        fprintf(stderr, "mergedImage failed\n");
        return 1;
      }
      if (!writeRaw(argv[i + 1], img.rgba)) return 1;
      printf("  merged -> %s (%u x %u RGBA8)\n", argv[i + 1], img.width, img.height);
      return 0;
    }
    if (strcmp(argv[i], "--layer") == 0 && i + 2 < argc) {
      const int idx = atoi(argv[i + 1]);
      clip::Image img;
      if (!f.layerImage(idx, clip::IMAGE_MODE_MASKED, img)) {
        fprintf(stderr, "layerImage(%d) failed\n", idx);
        return 1;
      }
      if (!writeRaw(argv[i + 2], img.rgba)) return 1;
      printf("  layer %d -> %s (%u x %u RGBA8)\n", idx, argv[i + 2],
             img.width, img.height);
      return 0;
    }
    if (strcmp(argv[i], "--region") == 0 && i + 5 < argc) {
      clip::Rect r;
      r.x = atoi(argv[i + 2]); r.y = atoi(argv[i + 3]);
      r.w = atoi(argv[i + 4]); r.h = atoi(argv[i + 5]);
      clip::Image img;
      if (!f.layerRegion(atoi(argv[i + 1]), r, clip::IMAGE_MODE_MASKED, img)) {
        fprintf(stderr, "layerRegion failed\n");
        return 1;
      }
      printf("  region (%d,%d %dx%d) -> %u x %u\n", r.x, r.y, r.w, r.h,
             img.width, img.height);
      if (i + 6 < argc && !writeRaw(argv[i + 6], img.rgba)) return 1;
      return 0;
    }
    if (strcmp(argv[i], "--dump-offscreen") == 0 && i + 2 < argc) {
      const long long id = atoll(argv[i + 1]);
      std::vector<uint8_t> rgba;
      uint32_t w = 0, h = 0;
      if (!f.readOffscreen(id, rgba, w, h)) {
        fprintf(stderr, "readOffscreen(%lld) failed\n", id);
        return 1;
      }
      if (!writeRaw(argv[i + 2], rgba)) return 1;
      printf("  offscreen #%lld -> %s (%u x %u RGBA8)\n", id, argv[i + 2], w, h);
      return 0;
    }
  }

  printf("--- layer tree ---\n");
  for (int r : f.roots()) dumpTree(f, r, 0);
  return 0;
}
