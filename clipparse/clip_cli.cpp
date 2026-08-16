// clipparse のスモークテスト CLI。
//
//   clip_cli file.clip           構造をダンプ
//   clip_cli file.clip --check   全ブロックの構造アサーションを回す
//   clip_cli file.clip --dump-offscreen ID out.raw   RGBA8 を生で書き出す
//
// Python 版 (tools/clip_probe.py) と同じ結果になることを確認するために使う。

#include "clipfile.h"
#include <cstdio>
#include <cstring>
#include <string>

static void dumpTree(const clip::ClipFile &f, int idx, int depth) {
  const auto &L = f.layers();
  const clip::LayerInfo &li = L[size_t(idx)];
  printf("%*s#%lld %-20s type=%-5lld folder=%lld vis=%lld comp=%-3lld opa=%lld/256",
         depth * 2, "", (long long)li.mainId, li.name.c_str(),
         (long long)li.type, (long long)li.folder, (long long)li.visibility,
         (long long)li.composite, (long long)li.opacity);
  const int64_t off = f.topOffscreen(li.mainId);
  if (off) {
    if (const clip::OffscreenAttr *a = f.attribute(off)) {
      printf("  offscreen=#%lld %ux%u blocks=%zu %s(cm%u,nch%u,bd%u)",
             (long long)off, a->width, a->height, a->blockSizes.size(),
             f.hasPixels(off) ? "pixels " : "NOPIX ",
             a->colorMode, a->numChannels, a->bitDepth);
    }
  }
  printf("\n");
  for (int c : li.children) dumpTree(f, c, depth + 1);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: clip_cli file.clip [--check] "
                    "[--dump-offscreen ID out.raw]\n");
    return 2;
  }
  clip::ClipFile f;
  if (!f.load(argv[1])) {
    fprintf(stderr, "load failed: %s\n", f.error().c_str());
    return 1;
  }
  printf("%s\n", argv[1]);
  printf("  canvas %lld x %lld  root=#%lld  layers=%zu\n",
         (long long)f.canvasWidth(), (long long)f.canvasHeight(),
         (long long)f.rootLayer(), f.layers().size());

  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--check") == 0) {
      std::string report;
      const bool ok = f.checkAll(&report);
      printf("--- check ---\n%s", report.c_str());
      printf("  => %s\n", ok ? "OK" : "NG");
      return ok ? 0 : 1;
    }
    if (strcmp(argv[i], "--dump-offscreen") == 0 && i + 2 < argc) {
      const long long id = atoll(argv[i + 1]);
      std::vector<uint8_t> rgba;
      uint32_t w = 0, h = 0;
      if (!f.readOffscreen(id, rgba, w, h)) {
        fprintf(stderr, "readOffscreen(%lld) failed\n", id);
        return 1;
      }
      FILE *fp = fopen(argv[i + 2], "wb");
      if (!fp) { fprintf(stderr, "cannot write %s\n", argv[i + 2]); return 1; }
      fwrite(rgba.data(), 1, rgba.size(), fp);
      fclose(fp);
      printf("  offscreen #%lld -> %s (%u x %u RGBA8)\n", id, argv[i + 2], w, h);
      return 0;
    }
  }

  const int root = f.layerIndex(f.rootLayer());
  if (root >= 0) {
    printf("--- layer tree ---\n");
    dumpTree(f, root, 0);
  }
  return 0;
}
