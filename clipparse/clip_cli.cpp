// clipparse のスモークテスト CLI。
//
//   clip_cli file.clip                          構造をダンプ
//   clip_cli file.clip --check                  全ブロックの構造アサーション
//   clip_cli file.clip --merged out.raw         合成結果を RGBA8 で書き出す
//   clip_cli file.clip --layer N out.raw        レイヤ N を RGBA8 で書き出す
//   clip_cli file.clip --region N x y w h [out] レイヤ N の一部だけ読む
//   clip_cli file.clip --dump-offscreen ID out.raw
//
// Python 版 (tools/) と同じ結果になることを確認するために使う。

#include "clipfile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: clip_cli file.clip [--check] [--merged out.raw]\n"
            "                         [--layer N out.raw]\n"
            "                         [--region N x y w h [out.raw]]\n"
            "                         [--dump-offscreen ID out.raw]\n");
    return 2;
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
