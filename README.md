# clipparse

[日本語版 README](README.ja.md) — Japanese version of this page.

C++17 library for CLIP STUDIO PAINT `.clip` files — **reads, composites, edits and
writes them** — with pybind11-based Python bindings on PyPI.

- **Lazy I/O.** Parsing touches only the embedded SQLite metadata. Pixels are
  decompressed per **256x256 block**, on demand, straight out of the mmapped file.
- **Partial reads.** `layer_region()` expands only the tiles that overlap the
  rectangle you ask for — something the row-RLE format of PSD cannot do.
- **Compositing.** All 27 CLIP blend modes, folders (including pass-through),
  masks, clipping and 5 kinds of adjustment layer, matched pixel-for-pixel against
  the preview CLIP STUDIO itself stores in the file.
- **Writing.** An unmodified round-trip is **byte-identical** (sha256, even for a
  60 MB file). Editing layer attributes, replacing pixels, adding/removing layers
  and rebuilding a canvas from scratch are all verified in **CLIP STUDIO PAINT PRO
  5.0.4** on real hardware.
- **CLIP to PSD and back.** Round-trip conversion via
  [psdparse](https://github.com/wamsoft/psdparse), in both C++ and Python.
- **No runtime dependencies.** zlib and sqlite3 are compiled in; the Python wheel
  is a single extension module.

The format was reverse-engineered from real files; what was verified by measurement
and what is still inferred are kept apart in [docs/CLIP_FORMAT.md](docs/CLIP_FORMAT.md).

## Install (Python)

```bash
pip install clipparse
```

Wheels are published for Python 3.9-3.14 (free-threaded builds included) on
Linux / Windows / macOS (x86_64 + arm64). From source — a C++17 compiler and
CMake 3.16+ is all you need, **no package manager**:

```bash
pip install .
```

## Quick start

```python
import clipparse

f = clipparse.ClipFile()
f.load("artwork.clip")

print(f.width, f.height, f.resolution)         # canvas size in pixels, DPI
for layer in f.layers:                          # flat list, bottom-to-top
    print(layer.index, layer.name, layer.opacity, layer.is_group)

bgra = f.merged_image()                         # every layer composited, BGRA bytes
one  = f.layer_image(2)                         # one layer, BGRA bytes
part = f.layer_region(2, 100, 120, 64, 48)      # only the overlapping tiles

png, w, h = f.preview_png()                     # the preview CLIP STUDIO stored
```

Pixels always come back as **BGRA bytes with straight (un-premultiplied) alpha**,
the same convention psdparse uses:

```python
from PIL import Image
img = Image.frombytes("RGBA", (f.width, f.height), f.merged_image())
b, g, r, a = img.split()
Image.merge("RGBA", (r, g, b, a)).save("merged.png")
```

Editing. The writer addresses layers by **`Layer.MainId`** (`layer.main_id`), not
by the list index used for reading:

```python
w = clipparse.ClipWriter()
w.load("artwork.clip")

w.set_layer_attr(main_id, name="renamed", opacity=128)   # opacity is 0..256 here
w.set_pixels(main_id, bgra, f.width, f.height)           # replace a layer's pixels
new_id = w.add_layer(main_id, "new layer", bgra, f.width, f.height)
w.delete_layer(other_id)

w.save("out.clip")
assert clipparse.validate("out.clip") == []              # run before opening in CSP
```

Full reference: **[docs/PYTHON_API.md](docs/PYTHON_API.md)**
([日本語](docs/PYTHON_API.ja.md)).

## Command-line tools

The Python bindings cover the library; the scripts under `tools/` are the
reference implementation and the day-to-day utilities. They live in the
repository — they are not part of the wheel.

```
# structure dump — chunk layout, tables, layer tree, block list (stdlib only)
python tools/clip_probe.py file.clip [--blocks]

# lazy-reference prototype: composite and compare against a reference PNG
python tools/clip_lazy_demo.py file.clip -o out.png --compare reference.png

# writing (an unmodified round-trip must be byte-identical)
python tools/clip_write.py roundtrip in.clip out.clip
python tools/clip_write.py set       in.clip out.clip --layer 5 --opacity 64 --composite 2
python tools/clip_write.py setpixels in.clip out.clip --layer 3 --png patch.png
python tools/clip_write.py addlayer  in.clip out.clip --copy-from 3 --name new --png patch.png

# referential-integrity check — ALWAYS run this before opening a written file in CSP
python tools/clip_validate.py out.clip

# CLIP <-> PSD (needs the psdparse Python bindings)
python tools/clip_to_psd.py input.clip output.psd  --verify
python tools/psd_to_clip.py input.psd  output.clip --verify
```

`clip_probe.py` needs nothing but the standard library; `clip_lazy_demo.py` needs
numpy, plus Pillow when comparing.

## Build (C++ library / CLI)

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

build\clipparse\Release\clip_cli.exe file.clip --check
build\clipparse\Release\clip_cli.exe file.clip --validate
build\clipparse\Release\clip_cli.exe in.clip --set 5 --opacity 64 --out out.clip
build\clipparse\Release\clip_cli.exe in.clip --set-pixels 3 rgba.raw out.clip
build\clipparse\Release\clip_cli.exe in.clip --add-layer  3 rgba.raw out.clip --name new
```

The only dependencies are **zlib and sqlite3**, both fetched from source by CMake
(`FetchContent`), so vcpkg and friends are unnecessary. SQLite is attached with
`sqlite3_deserialize(..., SQLITE_DESERIALIZE_READONLY)` directly on the mmapped
bytes — no temporary file is ever created.

```cpp
clip::ClipFile f;
f.load("artwork.clip");
clip::Image img;
f.mergedImage(img);                       // RGBA8, straight alpha

clip::ClipWriter w;
w.load("artwork.clip");
w.addLayer(3, "new layer", rgba, 300, 400);
w.save("out.clip");
```

C++ and Python produce **byte-identical chunk payloads** when writing, down to the
zlib output; that equivalence is what the test suite checks.

## CLIP to PSD conversion

`examples/clipconv/` is a standalone command that links both clipparse and
psdparse, using nothing but their public APIs.

```powershell
cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
cmake --build build-conv --config Release

build-conv\Release\clipconv.exe in.clip out.psd  --verify
build-conv\Release\clipconv.exe in.psd  out.clip --verify
```

Layer pixels, the folder tree and blend modes survive the round-trip; masks and
clipping are baked into alpha, and adjustment/vector layers are not exported.
Details in [examples/clipconv/README.md](examples/clipconv/README.md).

## What works, and what does not

| | |
|---|---|
| Reading | RGBA / gray / monochrome / mask planes, folders, masks, clipping, text and rasterized vector layers |
| Compositing | 27 blend modes, pass-through folders, 5 adjustment-layer kinds. Of 28 samples, 13 are pixel-exact against CSP's own preview and 22 are within rounding error |
| Writing | attributes, pixel replacement, add/delete layer, canvas rebuild — all confirmed in CSP 5.0.4 |
| Not supported | vector layers (a brush engine would be needed), some adjustment kinds (levels, colour balance, posterize, gradient map) |

Writing has traps a tolerant reader cannot see — per-table storage types, a
checksum CSP actually verifies, a mipmap count that crashes it when wrong. They
were flushed out over five rounds of testing on real CLIP STUDIO and are checked
mechanically by `clipparse.validate()` / `clip_cli --validate`.
**Run the validator before opening anything you wrote.**

## Tests

```powershell
python -m pytest tests -q
```

The tests cross-check the C++ extension against the pure-Python reference
implementation (pixels must match byte for byte) and the write path against
CLIP STUDIO's own rules. Real `.clip` samples are not committed — see
[docs/STATUS.md](docs/STATUS.md) for what goes into `samples/`.

## How the format works, in five lines

```
[CSFCHUNK header][CHNKHead][CHNKExta ...][CHNKSQLi][CHNKFoot]
                            ^ pixel data  ^ all metadata (a SQLite3 database)
```

`CHNKHead.binary_section_size` points straight at the SQLite chunk, so the first
64 bytes are enough to reach the metadata. From there, `ExternalChunk.Offset` plus
the prefix sum of `Offscreen.Attribute.BlockSize[]` gives the absolute position of
any pixel block — the binary area is never scanned.

## Documentation

| | |
|---|---|
| [docs/PYTHON_API.md](docs/PYTHON_API.md) ([ja](docs/PYTHON_API.ja.md)) | Python API reference |
| [docs/CLIP_FORMAT.md](docs/CLIP_FORMAT.md) | `.clip` format specification, measured facts kept apart from inferred ones |
| [docs/DESIGN.md](docs/DESIGN.md) | Design of the lazy-reference scheme, the API shared with psdparse, roadmap |
| [docs/STATUS.md](docs/STATUS.md) | Development status, how to resume, what is next |
| [docs/CLIP_TOOLS_REPORT.md](docs/CLIP_TOOLS_REPORT.md) | Feedback for clip-tools: three reproducible bugs and two spec corrections |

## Credits

- [animeops/clip-tools](https://github.com/animeops/clip-tools) — the Python
  implementation this analysis started from.
- [psdparse](https://github.com/wamsoft/psdparse) — the library whose design
  clipparse follows.

## License

MIT — see [LICENSE](LICENSE).
