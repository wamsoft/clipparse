# Report for animeops/clip-tools

Findings from independently re-deriving the CLIP format against 4 files, while
building a C++ lazy-loading reader. Everything below was reproduced with
clip-tools' own code where applicable.

Sections 1 and 3 are bugs. Sections 2 and 4 are corrections to
`clip_tools/clip.md` that also unlock a useful capability (random access).

**Samples used**

| file | size | layers | offscreens | notes |
|---|---|---|---|---|
| `tests/test_data/test000.clip` | 1.1 MB | 3 | 24 | from this repo |
| production A | 60 MB | 65 | 488 | adjustment layers + masks + folders |
| production B | 63 MB | 452 | 3144 | `Track` / `TimeLine` (binc chunks) |
| production C | 91 MB | 206 | 1877 | vector + text + brushes + rulers |

164,562 block sub-records were walked in total.

Sections 1-5 were written against those four files. Section 6 adds findings from
13 further files saved by CSP for this purpose (grayscale / monochrome canvases,
and one file per feature: text, vector, mask, clipping, folder, adjustment layer,
opacity, blend modes).

---

## 1. `process_offscreen_attributes` raises on mask/selection offscreens

**Severity: crash, and (via §3) wrong pixel data attributed to a layer.**

`clip_tools/structs/offscreen_attributes.py` reads the `InitColor` trailer as a
fixed 16 bytes whenever `has_color == 1`:

```python
    if has_init_color:
        extra, pos = read_binary_spec(attribute, uint4_spec, pos)
```

The payload is not fixed-length. The third element of the `init_color` quad is a
**count**, and the trailer is that many `u32`s:

```
u32[4] init_color = (has_color, packed_rgba, num_extra, num_channels)
u32[num_extra] init_color_extra
```

Cross-check: `section_sizes[2] == 42 + 4 * num_extra` holds in every sample.

Offscreens with `num_channels == 0` (layer-mask / selection planes) have
`has_color == 1` but `num_extra == 0`, so no trailer follows. The current code
consumes 16 bytes that belong to the next section and then fails:

```
ValueError: Invalid attribute: missing BlockSize header
```

### Reproduction

Running clip-tools' `process_offscreen_attributes` verbatim over every
`Offscreen.Attribute` blob:

```
production A : 14 / 488  raise ValueError
production B :  0 / 3144
production C :  0 / 1877
test000.clip :  0 / 24
```

The 14 failures in production A are the layer-mask mipmap chains of two
adjustment layers (`LayerType == 4098`), named トーンカーブ 1 / カラーバランス 1.
One offending blob (`Offscreen.MainId = 132`):

```
section_sizes = (16, 102, 42, 802)      # 42 = no trailer; 802 = 34 + 4*192 blocks
Parameter     : 3072x4096, cols=12 rows=16, (color_mode, alpha_flag, num_channels,
                bit_depth) = (1, 1, 0, 1)
InitColor     : magic=20, quad=(1, 0xFFFFFFFF, 0, 4)   <- has_color=1, num_extra=0
```

Two of the 14 (`MainId` 132 and 136) **do have chunk data**, so they reach
`process_layer_blocks` in the normal pipeline — this is not a latent path.

### Suggested patch

```python
    (initcolor_magic,), pos = read_binary_spec(attribute, uint_spec, pos)
    init_color_quad, pos = read_binary_spec(attribute, uint4_spec, pos)
    has_init_color = bool(init_color_quad[0])
    init_color = init_color_quad[1]
    num_extra = init_color_quad[2]

    init_color_extra = None
    init_color_extra_raw = None
    if num_extra:
        extra, pos = read_binary_spec(
            attribute, struct.Struct(f">{num_extra}I"), pos)
        init_color_extra = tuple(min(255, v >> 24) for v in extra)
        init_color_extra_raw = extra
```

This also answers open question 3 in `clip.md` ("`init_color_extra` (16 bytes) —
only present when `has_color == 1`"): the length is `4 * num_extra`, and
`num_extra` tracks the channel count (4 for RGBA, 0 for single-plane offscreens).

---

## 2. `BlockSize[i]` is the sub-record length, not the compressed size

`clip.md` currently says:

> `block_sizes` — `nblocks × u32` — the compressed byte size of each 256×256
> block's data.
> - Uniform-colored blocks (like a white paper fill) compress to ~104 bytes each.

Measured, the value is the **total length of the block's sub-record inside the
chunk stream**, including framing. The "~104 bytes" figure is not a compression
result — it is the fixed length of a sub-record that carries no pixels at all:

```
empty block      : 4 + 4 + 38 + 20              + 4 + 34 = 104 bytes exactly
block with data  : 4 + 4 + 38 + 20 + 8 + clen   + 4 + 34 = clen + 112
```

so `compressed_size == BlockSize[i] - 112`, and `BlockSize[i] == 104` means the
block is empty.

Verified for all 164,562 blocks across the four samples: the `record_size` field
read from the stream equals `Attribute.BlockSize[i]` in every case, with no
exceptions.

### Why this matters

`sum(BlockSize[0:i])` is the byte offset of block `i` within the chunk payload.
Combined with `ExternalChunk.Offset` (which points at the `CHNKExta` header;
payload starts at `Offset + 72`), **any single block can be located and
decompressed without scanning the binary region at all**:

```python
data_start = external_chunk_offset + 16 + 56
block_off  = data_start + sum(attr.block_sizes[:i])
# 74-byte record header, then `BlockSize[i] - 112` bytes of zlib
```

That turns `process_chunk_binary`'s eager full-region walk into an optional
integrity check, and makes partial reads (one tile of a 3072x4096 layer)
proportional to the tile rather than to the layer.

### Full sub-record layout (verified)

```
u32 BE  record_size            # includes this field; == Attribute.BlockSize[i]
u32 BE  name_length = 19
utf16be "BlockDataBeginChunk"
u32 BE  block_index
u32 BE  decompressed_size
u32 BE  block_width  = 256
u32 BE  block_height = 256
u32 BE  has_content
  if has_content:
    u32 BE  section_size       # == compressed_size + 4
    u32 LE  compressed_size    # little-endian, unlike everything around it
    u8[compressed_size]  zlib stream
u32 BE  name_length = 17
utf16be "BlockDataEndChunk"    # trailer, inside the same record
```

`BlockStatus` / `BlockCheckSum` follow the block list and are framed
*differently* — they have no `record_size` prefix and start straight at
`name_length`. (The existing heuristic in `chunk.py` — testing whether the
second u32 equals the first 4 bytes of `"BlockDataBeginChunk"` — happens to
handle this, but the asymmetry is worth stating explicitly.)

`decompressed_size` is stored per block and is a function of `num_channels`:

| `num_channels` | `(color_mode, alpha_flag, bit_depth)` | decompressed | layout |
|---|---|---|---|
| 4 | `(33, 1, 5)` | 327,680 | `(block_h + 64, block_w, 4)` — BGR + folded alpha plane |
| 1 | `(17, 1, 2)` | 131,072 | `(block_h * 2, block_w)` — the "brush?" case in `layer_blocks.py` |
| 0 | `(1, 1, 1)` | 65,536 | `(block_h, block_w)` — mask / selection |

This closes `clip.md` open question 1: the `(33, 1, ?, 5)` tuple is *not*
constant across files — it covaries with the document's colour mode. See §6 for
the full table, measured against grayscale and monochrome files.

---

## 3. `process_clip_data` reuses the previous layer's image when a parse fails

`clip_tools/processing.py`:

```python
            try:
                processed_layer_arr = process_layer_blocks(blocks, offscreen)
            except Exception:
                logger.error(
                    f"Error processing layer: {key} in {table_name} {column_name}"
                )

            if DEBUG:
                save_debug_layer_image(processed_layer_arr, name, key, "raster")
            ...
            raster_dict[layer_id] = LayerEntry(type="raster", image=processed_layer_arr)
```

`processed_layer_arr` is not reset in the `except` branch, so after a failure the
loop continues and assigns **the previous iteration's array** to this layer
(or raises `UnboundLocalError` if the very first iteration failed). The logged
error is the only signal, and the resulting `LayerEntry` looks valid downstream.

With §1 fixed this stops firing on these samples, but the failure mode is worth
closing regardless — e.g. `continue` after logging, or re-raise.

---

## 4. Render and layer-mask mipmap chains are indistinguishable by `(LayerId, ThisScale)`

A layer with a mask has **two** `Mipmap` chains, and both are in `MipmapInfo`
with the same `LayerId` and both starting at `ThisScale == 100.0`:

```
Layer.LayerRenderMipmap    -> Mipmap.MainId  -> Mipmap.BaseMipmapInfo -> MipmapInfo chain
Layer.LayerLayerMaskMipmap -> Mipmap.MainId  -> Mipmap.BaseMipmapInfo -> MipmapInfo chain
Mipmap(MainId, LayerId, MipmapCount, BaseMipmapInfo)
MipmapInfo(MainId, LayerId, ThisScale, Offscreen, NextIndex)
```

Example from production A (layer 61):

```
Mipmap 63 (render) LayerId=61 MipmapCount=6 BaseMipmapInfo=68
Mipmap 64 (mask)   LayerId=61 MipmapCount=7 BaseMipmapInfo=69
MipmapInfo 68 LayerId=61 ThisScale=100.0 Offscreen=130 Next=377
MipmapInfo 69 LayerId=61 ThisScale=100.0 Offscreen=132 Next=382   <- mask
```

`process_clip_data` classifies an offscreen as a full-resolution raster with

```python
if mipmapinfo["ThisScale"] != 100.0:   # -> "mipmap", skipped
```

which admits the mask chain's top level as if it were the layer's raster. In
production A, layer 7 (`塗り`) has chunk data on its **mask** 100% offscreen and
none on its render 100% offscreen — so the mask image becomes that layer's
`raster_dict` entry.

A layer with data on both would instead hit
`raise Exception(f"Layer {layer_id} already processed")` (inferred from the code;
not present in these four samples).

Resolving the chain from `Layer.LayerRenderMipmap` → `Mipmap.BaseMipmapInfo` and
then following `MipmapInfo.NextIndex` removes the ambiguity, and gives the mask
image a proper home at the same time.

Related: mask chains can be longer than the documented 5–6 levels — the example
above has 7 (down to `ThisScale = 1.5625`).

Also, the thumbnail is reachable via `Layer.LayerRenderThumbnail` /
`LayerLayerMaskThumbnail` → `LayerThumbnail.ThumbnailOffscreen`, rather than by
"not in `MipmapInfo`" — because **"not in `MipmapInfo`" is not the same set**.

There is a third category of offscreen that is referenced by neither
`MipmapInfo` nor `LayerThumbnail`, and by no foreign key at all — it is reachable
only through `Offscreen.LayerId`. These carry the rendered raster of object
layers (text / vector), cropped to the object's bounding box:

```
                   Offscreen   mipmap levels   thumbnails   neither
tama.clip                488             419           69         0
test000.clip              24              20            4         0
text.clip                 22              15            5         2  (both have data)
haruse-ja.clip          1877            1664          207         6  (all have data)
nazoani01_ja.clip       3144            2263          450       431  (all have data)
```

In `text.clip`, the two text layers' 100% mipmaps have **no** chunk data; instead
two unreferenced offscreens sized 31x151 and 194x35 — the text bounding boxes —
carry it. So a text or vector layer does not store a canvas-sized raster at all,
and `process_clip_data`'s "not in `MipmapInfo`" → `"other"` bucket silently
discards the only rendered copy of that layer.

---

## 6. Grayscale / monochrome layers: the two planes are (alpha, value), not (value, alpha)

`clip_tools/structs/layer_blocks.py` handles `num_channels == 1` as:

```python
        elif num_channels == 1:
            # Brush?
            shape = [block_height * 2, block_width]
            # TODO: Check why this is... only seems to happen for brushes
            temp_img = np.frombuffer(block_data, dtype=dt).reshape(shape)
            main_img = np.zeros((block_height, block_width, 2), dtype=np.uint8)
            main_img[..., 0] = temp_img[0:block_height, :]
            main_img[..., 1] = temp_img[block_height:, :]
            main_img[..., 1] = 255
```

This is not a brush-only case: it is **how every layer raster in a grayscale or
monochrome document is stored**. And the plane order is the other way round —
**plane 0 is alpha, plane 1 is the value**. The code above takes plane 0 as the
value and then overwrites plane 1 with 255, so a grayscale layer decodes to its
own alpha channel rendered as gray.

Measured on two files saved by CSP with 基本表現色 = グレー / モノクロ. Compositing
`value` over the white paper layer using `plane0` as alpha reproduces the file's
own `CanvasPreview` **exactly** (max channel difference 0 over 120,000 pixels);
the other order gives mean errors of 47 and 108 respectively.

The full set of plane layouts observed (256x256 blocks):

| color_mode | nch | bit_depth | plane_bytes | decompressed | layout |
|---|---|---|---|---|---|
| 33 | 4 | 5 | 65,536 | 327,680 | `(bh+64, bw, 4)`, BGR + folded alpha |
| 17 | 1 | 2 | 65,536 | 131,072 | 8bpp, plane0 = alpha, plane1 = gray value |
| 17 | 1 | 1 | 8,192 | 16,384 | **1bpp**, plane0 = alpha, plane1 = value, row stride = `row_bytes` (32) |
| 1 | 0 | 1 | 0 | 65,536 | single 8bpp plane (mask / selection) |

Two consequences worth noting:

- **Monochrome documents store the 100% mipmap at 1 bit per pixel**, while the
  reduced mipmap levels of the same layer are 8bpp gray `(17, 1, 1, 2)`. The
  current code would misread the 1bpp level as 8bpp.
- Grayscale/monochrome affects **layer rasters only**. The paper layer, the root
  folder and every thumbnail stay RGBA `(33, 1, 4, 5)` even in those documents.

The `Canvas` row agrees: `CanvasDefaultColorTypeIndex` is 0/1/2 for
colour/gray/mono, and `CanvasDefaultChannelOrder` carries the same 33/17 code
that appears as `color_mode` in the `Attribute` blob.

Sizes follow a single rule, `decompressed == (plane_count + 1) * plane_bytes`
(the `Parameter` section's `geom[1]` and `geom[0]`), which held for 16,814 of
16,916 blocks measured; the exceptions are mask planes, whose geometry fields are
zeroed. Reading the per-block `decompressed_size` field from the sub-record
header (§2) avoids the special case entirely.

---

## 7. Smaller notes

- **`BlockSize[]` having non-104 entries does not mean the chunk exists.**
  Offscreens with a fully populated size table but no `CHNKExta` at all:
  332 / 700 / 1293 in the three production files (and the root folder's 100%
  mipmap in `test000.clip`). `ExternalChunk` membership is the only reliable
  presence test. `clip.md` mentions empty `{}` entries in `clip_data`; this is
  the SQLite-side counterpart.

- **`ExternalTableAndColumnName` lists tables that do not exist in the file** —
  9 of 10 rows in `test000.clip`, 6–9 of 10 in the production files. The existing
  "skip missing columns" guard should be a "skip missing tables *or* columns"
  guard.

- **`CHNKHead.binary_section_size` equals the offset of the `CHNKSQLi` chunk
  header** in all four samples. Reading the first 64 bytes is enough to jump
  straight to the database; scanning for the `SQLite format 3` magic
  (`split_clip_binary`) is not required, and the chunk length gives the exact DB
  size rather than "everything to EOF".

- **`LayerType` value 8720** appears in production B (2 layers) and is not in the
  `LayerKind` enum.

- **Straight (non-premultiplied) alpha** for raster offscreens: compositing
  `test000.clip`'s three layers bottom-up with straight `src-over` reproduces
  `test000.png` exactly (max channel difference 0 over 980,000 pixels). The 4th
  byte of the colour section is unused (all zero in the blocks inspected); the
  alpha that matters is the folded plane in rows `[0:64]`, as
  `encode_blocks.py` already documents.

---

## Reproduction code

The tooling used here is at `tools/clip_probe.py` and `tools/clip_lazy_demo.py`
in the reporting repository. `clip_probe.py` depends only on the standard library
and prints the chunk layout, table inventory, layer tree, every
`Offscreen.Attribute` field, and every block sub-record with its assertions
(`record_size == BlockSize[i]`, `section_size == compressed_size + 4`,
`len(decompressed) == decompressed_size`, and the size formula).

The §1 reproduction loads `clip_tools/structs/offscreen_attributes.py` verbatim
(stubbing only `clip_tools.utils.read_binary_spec`, copied unchanged, to avoid
the pandas/cv2 import chain) and runs it over every `Offscreen.Attribute` blob.

Thanks for `clip.md` — the reverse-engineering notes at the end of it were what
made this tractable.
