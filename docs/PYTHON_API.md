# clipparse Python API Reference

[日本語版](PYTHON_API.ja.md) — Japanese version of this page.

`clipparse` is a single C++ extension module with **no Python dependencies**. It
reads CLIP STUDIO PAINT `.clip` files lazily (pixels are decompressed per
256x256 block, on demand), composites them, and writes them back in a form
CLIP STUDIO accepts.

```bash
pip install clipparse
```

```python
import clipparse
```

The module exposes five names:

| | |
|---|---|
| [`ClipFile`](#class-clipparseclipfile) | read a file: metadata, layer tree, pixels, composite |
| [`LayerInfo`](#class-clipparselayerinfo) | read-only view of one layer (never constructed directly) |
| [`OffscreenAttr`](#class-clipparseoffscreenattr) | decoded `Offscreen.Attribute` — geometry and per-block sizes |
| [`ClipWriter`](#class-clipparseclipwriter) | edit and save a file |
| [`validate()`](#clipparsevalidatepath) | check a written file before CLIP STUDIO sees it |

## Conventions you need before anything else

**Pixels are BGRA `bytes`, straight (un-premultiplied) alpha.** Every image
returned by this module is `bytes` of length `width * height * 4`, in B, G, R, A
order — the same convention psdparse uses. See [Pixel format](#pixel-format).

**Reading uses list indices, writing uses `main_id`.** `ClipFile` numbers layers
by their position in `f.layers`; `ClipWriter` addresses them by CLIP's own
`Layer.MainId`. Get one from the other with `f.layers[i].main_id`.

**The layer list is flat, bottom-to-top, contents before their folder.** Exactly
psdparse's ordering, so code written against one works on the other. The tree is
a derived view: `f.roots` and `f.children(i)`.

**Opacity has two scales.** CLIP stores 0..**256**. `layer.opacity` is rescaled
to 0..255 so it lines up with PSD; `layer.opacity_raw` is CLIP's own value, and
`ClipWriter.set_layer_attr(opacity=...)` expects that raw 0..256 scale.

**Paths are `str`** and are encoded as UTF-8 internally (converted to UTF-16 on
Windows before the file is mapped).

**One object, one thread.** `ClipFile` caches decoded attributes internally and
is not thread-safe; give each thread its own instance. Calls hold the GIL, so
threads will not overlap anyway — use processes if you want parallelism.

**Keep the `ClipFile` alive.** `LayerInfo` objects are views into the file that
produced them. Holding one after the `ClipFile` has been garbage-collected is
undefined behaviour.

---

## `class clipparse.ClipFile`

```python
f = clipparse.ClipFile()
```

### Loading

```python
f.load(path: str) -> bool
```

Memory-map the file and parse it. **Only the embedded SQLite metadata is read** —
even for a 90 MB illustration this is fast (a few tenths of a second) because no
pixel block is touched. Returns `False` on failure instead of raising; the reason
is in `f.error`.

```python
f.is_loaded   # bool  — True once a file with at least one layer is loaded
f.error       # str   — reason for the last failure, e.g. 'cannot open/map file'
```

### Canvas

```python
f.width        # int   — canvas width in PIXELS
f.height       # int   — canvas height in PIXELS
f.resolution   # float — DPI
```

`Canvas.CanvasWidth` inside the file is expressed in `CanvasUnit`, which is
millimetres in some real files, so these two properties are derived from the root
folder's 100% mipmap instead. They are always pixels.

### Layers

```python
f.layers       # list[LayerInfo] — flat, bottom-to-top, contents before their folder
f.roots        # list[int]       — indices of the top-level layers, bottom-to-top
f.children(i)  # list[int]       — direct children of layers[i]; pass -1 for the roots
```

`children()` raises `IndexError` for an index past the end.

```python
def walk(f, index=-1, depth=0):
    for i in f.children(index):
        layer = f.layers[i]
        print("  " * depth + layer.name)
        if layer.is_group:
            walk(f, i, depth + 1)
```

### Pixels

```python
f.layer_image(index: int, mode: str = "masked") -> bytes
```

The pixels of one layer, as BGRA bytes of length `layer.width * layer.height * 4`
— that is, the size of the layer's bounding box (`layer.left/top/width/height`),
not the canvas. Only the blocks that actually hold data are decompressed.

- `mode="masked"` (default) — the layer with its mask folded into alpha
- `mode="image"` — the layer only, mask ignored
- `mode="mask"` — the mask alone, as grayscale in BGRA

Returns `b""` for layers with no pixels of their own (folders). Raises
`IndexError` for a bad index, `ValueError` for a bad mode.

```python
f.layer_region(index: int, x: int, y: int, width: int, height: int,
               mode: str = "masked") -> bytes
```

The same, but only for the rectangle `(x, y, width, height)` **in canvas
coordinates**. Only the 256x256 blocks overlapping the rectangle are
decompressed, which is the one thing CLIP does that PSD's row-based RLE cannot.
Always returns `width * height * 4` bytes (areas outside the canvas come back
transparent); an empty rectangle returns `b""`.

```python
f.merged_image() -> bytes
```

Composite every visible layer bottom-to-top and return `width * height * 4` BGRA
bytes. Blend modes, folder nesting (including pass-through), masks, clipping and
adjustment layers are all applied. This is meant to reproduce what CLIP STUDIO
itself stored in the file — on the sample set, 13 of 28 files match it exactly and
22 match within rounding error.

```python
f.preview_png() -> tuple[bytes, int, int] | None
```

The preview image CLIP STUDIO saved inside the file (`CanvasPreview`), as
`(png_bytes, width, height)`, or `None` when the file has none. It is the
finished artwork as CLIP STUDIO rendered it — useful as ground truth — but it is
not always full canvas size.

### Low-level access

Rarely needed; these expose the storage layer that the properties above are
built on.

```python
f.top_offscreen(layer_main_id: int, mask: bool = False) -> int
```

`Offscreen.MainId` of the 100% mipmap level for a layer's image (or its mask when
`mask=True`). `0` when there is none.

```python
f.attribute(offscreen_id: int) -> OffscreenAttr | None
```

The decoded `Offscreen.Attribute` for that offscreen; `None` if unknown.

```python
f.check() -> tuple[bool, str]
```

Walk every block in the file and assert the structural invariants (block sizes,
offsets, plane layout). Returns `(ok, report)`; the report is a human-readable
summary. This checks that *reading* is consistent — for files you have *written*,
use [`validate()`](#clipparsevalidatepath) instead.

---

## `class clipparse.LayerInfo`

A read-only view of one layer, obtained from `ClipFile.layers`. Attribute names
match psdparse's `LayerInfo` wherever the concept exists in both formats.

| Attribute | Type | Notes |
|---|---|---|
| `index` | `int` | position in `ClipFile.layers` — what the reading API takes |
| `main_id` | `int` | CLIP's `Layer.MainId` — what the **writing** API takes |
| `layer_id` | `int` | same value as `main_id` (psdparse compatibility) |
| `name` | `str` | layer name |
| `name_unicode` | `str` | same as `name`; CLIP stores names as UTF-8, so unlike PSD there is no second raw/Unicode pair |
| `visible` | `bool` | |
| `opacity` | `int` | 0..255, rescaled from CLIP's 0..256 |
| `opacity_raw` | `int` | CLIP's raw `LayerOpacity`, 0..**256** |
| `fill_opacity` | `int` | always 255 (CLIP has no separate fill opacity) |
| `clipping` | `int` | 1 when the layer is clipped to the one below |
| `composite_raw` | `int` | raw `LayerComposite` — see [Blend modes](#blend-modes) |
| `is_group` | `bool` | layer folder |
| `is_filter` | `bool` | adjustment layer (no pixels of its own) |
| `is_text` | `bool` | text layer |
| `has_mask` | `bool` | |
| `transparency_protected` | `bool` | always `False` (not yet modelled) |
| `left`, `top`, `right`, `bottom` | `int` | bounding box on the canvas |
| `width`, `height` | `int` | derived from the bounding box; `0` for folders |
| `parent_index` | `int` | index of the enclosing folder, or `-1` at top level |
| `children` | `list[int]` | indices of the direct children, bottom-to-top; empty for non-folders |

The bounding box is where the layer's pixels live. For a normal raster layer it
is usually the whole canvas; for a text layer it is the text's bounding rectangle.

---

## `class clipparse.OffscreenAttr`

The decoded `Offscreen.Attribute` blob: how one raster is laid out in the file.
You need this only when working with blocks directly — geometry, colour mode and
the per-block sizes whose prefix sum locates any block in the binary area.

| Attribute | Type | Notes |
|---|---|---|
| `width`, `height` | `int` | logical size of this raster |
| `cols`, `rows` | `int` | block grid |
| `block_width`, `block_height` | `int` | almost always 256 x 256 |
| `color_mode` | `int` | 33 = RGBA, 17 = gray/monochrome, 1 = mask |
| `num_channels` | `int` | 4 / 1 / 0 |
| `bit_depth` | `int` | 5 = 8bpp RGBA, 2 = 8bpp, 1 = 1bpp |
| `plane_bytes` | `int` | bytes per plane |
| `has_init_color` | `bool` | whether an initial fill colour is stored |
| `init_color` | `int` | that colour, RGBA packed big-endian |
| `block_sizes` | `list[int]` | **total sub-record length** per block, not the compressed size. 104 = empty block; otherwise compressed length + 112 |

---

## `class clipparse.ClipWriter`

```python
w = clipparse.ClipWriter()
```

The writer loads the whole file into memory (so it is safe to save over the path
you loaded from), applies edits to the embedded SQLite database and the chunk
list, then recomputes every chunk offset on `save()`.

**An unmodified round-trip is byte-identical:**

```python
w.load("in.clip"); w.save("out.clip")     # sha256(out) == sha256(in)
```

Every method here raises `RuntimeError` on failure, with the library's error
message; `ValueError` is raised for a pixel buffer whose size does not match the
width and height you passed.

### Loading and saving

```python
w.load(path: str) -> bool                 # raises RuntimeError on failure
w.save(path: str) -> int                  # returns the number of bytes written
```

### `set_layer_attr` — layer attributes

```python
w.set_layer_attr(main_id: int, name: str | None = None, opacity: int = -1,
                 visible: int = -1, composite: int = -1, clipping: int = -1,
                 folder: int = -1) -> bool
```

Change one layer's attributes; `-1` (or `None` for `name`) leaves a field alone.
Only the SQLite database is touched, so no chunk moves and the file size does not
change.

- `opacity` — **0..256**, CLIP's own scale
- `visible` — 0 or 1
- `composite` — see [Blend modes](#blend-modes)
- `clipping` — 0 or 1
- `folder` — bit 0 = is a folder, bit 4 = collapsed

### `set_pixels` — replace a layer's pixels

```python
w.set_pixels(main_id: int, bgra: bytes, width: int, height: int) -> bool
```

Replace the 100% mipmap of a layer. **The buffer must cover the whole canvas** —
`width` and `height` have to equal the layer's 100% mipmap size, or you get
`RuntimeError: pixel size does not match the 100% mipmap`. The alpha plane is
re-folded, the blocks are re-compressed, `BlockSize[]` is rewritten and every
`ExternalChunk.Offset` after it is recomputed.

The now-stale thumbnail is dropped automatically so CLIP STUDIO regenerates it.
The canvas preview is **not** refreshed — see the [recipe](#replace-a-layers-pixels).

### `add_layer` / `delete_layer`

```python
w.add_layer(copy_from: int, name: str, bgra: bytes | None = None,
            width: int = 0, height: int = 0,
            after: int = -1, parent: int = 0) -> int
```

Add a layer by **cloning an existing one as a template** and replacing its ids,
links and pixels. (`Layer` has 57 columns whose CSP-expected defaults are largely
undocumented; copying a real row avoids guessing them.) Returns the new
`Layer.MainId`.

- `copy_from` — `main_id` of the template layer
- `bgra` — canvas-sized pixels, or `None` for a fully transparent layer
- `after` — `main_id` of the sibling to insert above; `-1` puts it on top
- `parent` — `main_id` of the enclosing folder; `0` means the canvas root

```python
w.delete_layer(main_id: int) -> bool
```

Removes the layer, its mipmap chain, its thumbnail and their chunks, unlinks it
from its siblings, and moves `Canvas.CanvasCurrentLayer` if it pointed at the
layer being deleted.

### `set_canvas_preview`

```python
w.set_canvas_preview(bgra: bytes, width: int, height: int) -> bool
```

Replace the image CLIP STUDIO displays the instant the file opens. A stale
preview makes the canvas look wrong until the user touches a layer, so refresh it
after any pixel edit.

### `resize_canvas`

```python
w.resize_canvas(width: int, height: int, dpi: float = 0.0) -> bool
```

Rebuild the canvas at a new size, growing or shrinking every mipmap chain to the
level count CLIP STUDIO uses for those dimensions. **All block data is dropped**;
the caller has to put pixels back with `set_pixels()` afterwards. This is what
`tools/psd_to_clip.py` uses to turn an empty template into a canvas of arbitrary
size.

### `set_external_id_seed`

```python
w.set_external_id_seed(seed: int) -> None
```

Fix the random seed used for new external chunk ids, so a test can produce
reproducible output.

---

## `clipparse.validate(path)`

```python
clipparse.validate(path: str) -> list[str]
```

Check referential integrity of a `.clip` on disk and return the problems found —
an **empty list means clean**. It looks for mipmap counts that disagree with the
chain length, cycles and orphan rows, wrong per-table storage types, and
references to layers that no longer exist.

**Run this before opening a written file in CLIP STUDIO.** The failures it
catches are exactly the ones a tolerant reader cannot see: the file reads back
perfectly in this library while CLIP STUDIO shows a blank layer, reports
"the layer image is damaged", or crashes during load.

```python
problems = clipparse.validate("out.clip")
if problems:
    raise SystemExit("\n".join(problems))
```

---

## Rules CLIP STUDIO enforces

These were found over five rounds of testing against CLIP STUDIO PAINT PRO 5.0.4.
`ClipWriter` handles all of them for the edits it performs; the list matters when
you go around it (raw SQLite via `tools/`, or your own writer).

1. `Offscreen.BlockData` is a **BLOB**, `ExternalChunk.ExternalID` is **TEXT** —
   the same 40-character id, opposite storage types. Get the first wrong and the
   layer opens fully transparent; get the second wrong and your `UPDATE` silently
   matches no rows.
2. `BlockCheckSum` must be **0**. CLIP STUDIO verifies non-zero checksums, and its
   algorithm is still unidentified; 0 means "no checksum" and passes.
3. `Mipmap.MipmapCount` must equal the number of levels in the chain, or
   CLIP STUDIO **crashes while loading**.
4. After changing pixels, drop the thumbnail's chunk *and* set the
   `LayerThumbnail.Thumbnail*NeedRefresh` columns to 50 — they are generation
   numbers, and merely deleting the data leaves the old thumbnail on screen.
5. Re-composite `CanvasPreview`; it is what the user sees at the moment the file
   opens.
6. `Canvas.CanvasCurrentLayer` must point at a layer that still exists.

---

## Blend modes

`layer.composite_raw` and `ClipWriter.set_layer_attr(composite=...)` use CLIP's
own numbering. All 27 were identified by measurement against CLIP STUDIO's own
composite; the formulas are in [CLIP_FORMAT.md §9](CLIP_FORMAT.md).

| | | | | | |
|---|---|---|---|---|---|
| 0 | Normal | 11 | Add | 21 | Difference |
| 1 | Darken | 12 | Add (Glow) | 22 | Exclusion |
| 2 | Multiply | 13 | Lighter color | 23 | Hue |
| 3 | Color burn | 14 | Overlay | 24 | Saturation |
| 4 | Linear burn | 15 | Soft light | 25 | Color |
| 5 | Subtract | 16 | Hard light | 26 | Luminosity |
| 6 | Darker color | 17 | Vivid light | 30 | Pass-through (folders only) |
| 7 | Lighten | 18 | Linear light | 36 | Divide |
| 8 | Screen | 19 | Pin light | | |
| 9 | Color dodge | 20 | Hard mix | | |
| 10 | Color dodge (Glow) | | | | |

The two "(Glow)" modes use the same formula as their plain counterparts but treat
alpha differently.

---

## Pixel format

Every image is `bytes`, 4 bytes per pixel, **B, G, R, A**, straight alpha, rows
top to bottom with no padding.

With NumPy:

```python
import numpy as np
a = np.frombuffer(f.merged_image(), np.uint8).reshape(f.height, f.width, 4)
rgba = a[..., [2, 1, 0, 3]]        # BGRA -> RGBA
```

With Pillow:

```python
from PIL import Image
img = Image.frombytes("RGBA", (f.width, f.height), f.merged_image())
b, g, r, a = img.split()
img = Image.merge("RGBA", (r, g, b, a))
```

Going the other way (Pillow/NumPy to `ClipWriter`), swap the channels back and
pass `bytes` — any object supporting the buffer protocol works, including a
NumPy array, as long as it is C-contiguous and the right size.

---

## Error model

| Situation | Behaviour |
|---|---|
| `ClipFile.load()` on a missing or malformed file | returns `False`, reason in `f.error` |
| Layer index out of range | `IndexError` |
| Bad `mode` string | `ValueError` |
| Pixel buffer length does not match `width * height * 4` | `ValueError` |
| Any `ClipWriter` operation that fails (unknown layer, wrong pixel size, save error) | `RuntimeError` carrying the library's message |
| `ClipFile.attribute()` for an unknown id | returns `None` |
| `ClipFile.preview_png()` when the file stores no preview | returns `None` |

---

## Recipes

### Export every layer as a PNG

```python
import clipparse
from PIL import Image

f = clipparse.ClipFile()
f.load("artwork.clip")

for layer in f.layers:
    if layer.is_group or not layer.width:
        continue
    data = f.layer_image(layer.index)
    img = Image.frombytes("RGBA", (layer.width, layer.height), data)
    b, g, r, a = img.split()
    Image.merge("RGBA", (r, g, b, a)).save(f"layer{layer.index:02d}.png")
```

### Read one tile of a huge canvas

```python
f = clipparse.ClipFile()
f.load("big.clip")                       # metadata only — nothing decompressed yet
tile = f.layer_region(7, 4096, 2048, 512, 512)   # only the overlapping blocks
```

This is the reason for the lazy design: the cost is proportional to the region
you ask for, not to the size of the file.

### Change layer attributes

```python
w = clipparse.ClipWriter()
w.load("in.clip")
w.set_layer_attr(main_id, name="line art", opacity=192, composite=2)  # multiply
w.save("out.clip")
assert clipparse.validate("out.clip") == []
```

Attribute-only edits rewrite nothing but the database, so this is cheap even on a
large file.

### Replace a layer's pixels

`set_pixels()` drops the stale thumbnail for you, but the canvas preview has to
be re-composited from the finished file — load the result, composite it, and
write the preview back:

```python
w = clipparse.ClipWriter()
w.load("in.clip")
w.set_pixels(main_id, bgra, canvas_w, canvas_h)
w.save("out.clip")

f = clipparse.ClipFile()                 # re-open the finished file
f.load("out.clip")
merged, cw, ch = f.merged_image(), f.width, f.height
del f                                    # release the mapping before writing

w2 = clipparse.ClipWriter()
w2.load("out.clip")
w2.set_canvas_preview(merged, cw, ch)
w2.save("out.clip")

assert clipparse.validate("out.clip") == []
```

### Add a layer

```python
w = clipparse.ClipWriter()
w.load("in.clip")
new_id = w.add_layer(template_main_id, "new layer", bgra, canvas_w, canvas_h,
                     after=below_main_id, parent=folder_main_id)
w.save("out.clip")
```

Pass `bgra=None` for an empty layer — CLIP STUDIO opens it, and the user can draw
on it and save normally.

---

## Not covered by the bindings

- **Vector layers.** CLIP stores no raster for them, so reproducing one needs a
  brush engine. Rasterizing the layer in CLIP STUDIO first works and matches
  exactly.
- **Some adjustment layers.** Brightness/contrast, tone curve, hue, invert and
  binarize are implemented; levels, colour balance, posterize and gradient map
  are not.
- **Text as text.** Text layers are read as their rendered raster; the string,
  font and layout are not exposed.
- **PSD conversion.** Not part of the wheel — it needs psdparse. Use
  `tools/clip_to_psd.py` / `tools/psd_to_clip.py` from the repository, or the
  `examples/clipconv` C++ command.

## Related, in the repository

`tools/imgdoc.py` puts a **psdparse-compatible facade** over `.clip`, so scripts
written against psdparse run unmodified on CLIP files:

```python
import imgdoc
doc = imgdoc.open("file.clip")      # a .psd path returns psdparse.PSDFile itself
doc.header.width, doc.header.height
for i in doc.roots:
    print(doc.layers[i].name_unicode, doc.layers[i].children)
doc.layer_image(0)                  # BGRA bytes
```

`layer_type` and `blend_mode` return psdparse's own enums, so comparisons such as
`layer.blend_mode == psdparse.BlendMode.MULTIPLY` work. It uses the C++ extension
when present and a pure-Python reference implementation otherwise
(`imgdoc.BACKEND` tells you which). It is **not** shipped in the wheel, because it
requires psdparse and the wheel has no dependencies — take it from the repository.
