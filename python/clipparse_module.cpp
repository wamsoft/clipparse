// clipparse の Python バインディング。
//
// psdparse の PSDFile / LayerInfo と**同じ名前・同じ意味**の読み取り面を出す。
// ただし enum は C++ 側で完結させ (int で返す)、psdparse の enum への変換は
// `tools/imgdoc.py` が行う。C++ モジュールが psdparse に依存しないようにするため。

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "clipfile.h"
#include "clipwrite.h"

namespace py = pybind11;

namespace {

  py::bytes toBgra(const clip::Image &img) {
    std::string out;
    out.resize(img.rgba.size());
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
      out[i + 0] = char(img.rgba[i + 2]);   // B
      out[i + 1] = char(img.rgba[i + 1]);   // G
      out[i + 2] = char(img.rgba[i + 0]);   // R
      out[i + 3] = char(img.rgba[i + 3]);   // A
    }
    return py::bytes(out);
  }

  std::vector<uint8_t> bgraToRgba(const uint8_t *src, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out;
    out.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
      out[i + 0] = src[i + 2];
      out[i + 1] = src[i + 1];
      out[i + 2] = src[i + 0];
      out[i + 3] = src[i + 3];
    }
    return out;
  }

  clip::ImageMode parseMode(const std::string &m) {
    if (m == "masked") return clip::IMAGE_MODE_MASKED;
    if (m == "image")  return clip::IMAGE_MODE_IMAGE;
    if (m == "mask")   return clip::IMAGE_MODE_MASK;
    throw py::value_error("mode must be 'masked', 'image' or 'mask'");
  }

  // クラス側から index を引けるようにするための軽い参照
  struct LayerRef {
    clip::ClipFile *file;
    int index;
  };

  const clip::LayerInfo &info(const LayerRef &r) {
    return r.file->layers()[size_t(r.index)];
  }

} // namespace

PYBIND11_MODULE(clipparse, m) {
  m.doc() = "CLIP STUDIO PAINT (.clip) lazy reader. "
            "Mirrors psdparse's read API; see tools/imgdoc.py for the "
            "psdparse-compatible facade.";

  py::class_<LayerRef>(m, "LayerInfo", "Read-only view of one layer.")
    .def_property_readonly("index", [](const LayerRef &r) { return r.index; })
    .def_property_readonly("main_id",
        [](const LayerRef &r) { return info(r).mainId; },
        "CLIP's Layer.MainId.")
    .def_property_readonly("name", [](const LayerRef &r) { return info(r).name; })
    .def_property_readonly("name_unicode",
        [](const LayerRef &r) { return info(r).name; },
        "Same as `name` — CLIP stores layer names as UTF-8, so there is no "
        "second raw/Unicode pair the way PSD has (Pascal name + luni).")
    .def_property_readonly("layer_id", [](const LayerRef &r) { return info(r).mainId; })
    .def_property_readonly("visible",
        [](const LayerRef &r) { return info(r).visibility != 0; })
    .def_property_readonly("opacity",
        [](const LayerRef &r) {
          const int64_t o = info(r).opacity;          // CLIP は 0..256
          return int(o * 255 / 256 > 255 ? 255 : o * 255 / 256);
        },
        "0..255, normalised from CLIP's 0..256 so it lines up with PSD.")
    .def_property_readonly("opacity_raw",
        [](const LayerRef &r) { return int(info(r).opacity); },
        "CLIP's raw LayerOpacity (0..**256**).")
    .def_property_readonly("fill_opacity", [](const LayerRef &) { return 255; })
    .def_property_readonly("clipping",
        [](const LayerRef &r) { return info(r).clipping ? 1 : 0; })
    .def_property_readonly("composite_raw",
        [](const LayerRef &r) { return int(info(r).composite); },
        "CLIP's raw LayerComposite (see docs/CLIP_FORMAT.md §9).")
    .def_property_readonly("is_group", [](const LayerRef &r) { return info(r).isGroup; })
    .def_property_readonly("is_filter",
        [](const LayerRef &r) { return info(r).isFilter; },
        "True for adjustment layers (they have no pixels of their own).")
    .def_property_readonly("is_text", [](const LayerRef &r) { return info(r).isText; })
    .def_property_readonly("has_mask", [](const LayerRef &r) { return info(r).hasMask; })
    .def_property_readonly("transparency_protected", [](const LayerRef &) { return false; })
    .def_property_readonly("left",   [](const LayerRef &r) { return info(r).bounds.x; })
    .def_property_readonly("top",    [](const LayerRef &r) { return info(r).bounds.y; })
    .def_property_readonly("width",  [](const LayerRef &r) { return info(r).bounds.w; })
    .def_property_readonly("height", [](const LayerRef &r) { return info(r).bounds.h; })
    .def_property_readonly("right",
        [](const LayerRef &r) { return info(r).bounds.x + info(r).bounds.w; })
    .def_property_readonly("bottom",
        [](const LayerRef &r) { return info(r).bounds.y + info(r).bounds.h; })
    .def_property_readonly("parent_index",
        [](const LayerRef &r) { return info(r).parent; },
        "Index of the enclosing folder, or -1 for top level.")
    .def_property_readonly("children",
        [](const LayerRef &r) { return info(r).children; },
        "Indices of the direct children, bottom-to-top. Empty for non-folders.")
    .def("__repr__", [](const LayerRef &r) {
        const auto &li = info(r);
        return "<clipparse.LayerInfo " + std::to_string(r.index) + " '" + li.name +
               "' " + std::to_string(li.bounds.w) + "x" + std::to_string(li.bounds.h) + ">";
      });

  py::class_<clip::OffscreenAttr>(m, "OffscreenAttr",
      "Decoded Offscreen.Attribute — geometry and per-block sizes.")
    .def_readonly("width", &clip::OffscreenAttr::width)
    .def_readonly("height", &clip::OffscreenAttr::height)
    .def_readonly("cols", &clip::OffscreenAttr::cols)
    .def_readonly("rows", &clip::OffscreenAttr::rows)
    .def_readonly("color_mode", &clip::OffscreenAttr::colorMode)
    .def_readonly("num_channels", &clip::OffscreenAttr::numChannels)
    .def_readonly("bit_depth", &clip::OffscreenAttr::bitDepth)
    .def_readonly("plane_bytes", &clip::OffscreenAttr::planeBytes)
    .def_readonly("block_width", &clip::OffscreenAttr::blockWidth)
    .def_readonly("block_height", &clip::OffscreenAttr::blockHeight)
    .def_readonly("has_init_color", &clip::OffscreenAttr::hasInitColor)
    .def_readonly("init_color", &clip::OffscreenAttr::initColor)
    .def_readonly("block_sizes", &clip::OffscreenAttr::blockSizes);

  py::class_<clip::ClipFile>(m, "ClipFile")
    .def(py::init<>())
    .def("load",
        [](clip::ClipFile &self, const std::string &path) {
          return self.load(path.c_str());
        },
        py::arg("path"),
        "Memory-map and parse. Only metadata (the embedded SQLite) is touched; "
        "pixels are decompressed per 256x256 block on demand.")
    .def_property_readonly("is_loaded",
        [](const clip::ClipFile &self) { return !self.layers().empty(); })
    .def_property_readonly("error",
        [](const clip::ClipFile &self) { return self.error(); })
    .def_property_readonly("width",
        [](const clip::ClipFile &self) { return self.canvasWidth(); },
        "Canvas width in **pixels**. Canvas.CanvasWidth is in CanvasUnit and "
        "may be millimetres, so this comes from the root folder's 100% mipmap.")
    .def_property_readonly("height",
        [](const clip::ClipFile &self) { return self.canvasHeight(); })
    .def_property_readonly("resolution",
        [](const clip::ClipFile &self) { return self.canvasResolution(); })
    .def_property_readonly("layers",
        [](clip::ClipFile &self) {
          std::vector<LayerRef> out;
          out.reserve(self.layers().size());
          for (int i = 0; i < int(self.layers().size()); i++)
            out.push_back(LayerRef{ &self, i });
          return out;
        },
        "Flat list, bottom-to-top, with a folder's contents before the folder "
        "itself — the same ordering psdparse uses.")
    .def_property_readonly("roots",
        [](const clip::ClipFile &self) { return self.roots(); },
        "Indices of the top-level layers, bottom-to-top.")
    .def("children",
        [](const clip::ClipFile &self, int index) {
          if (index < 0) return self.roots();
          if (index >= int(self.layers().size())) throw py::index_error();
          return self.layers()[size_t(index)].children;
        },
        py::arg("index"),
        "Direct children of layers[index]; pass -1 for the top level.")
    .def("layer_image",
        [](const clip::ClipFile &self, int index, const std::string &mode) {
          if (index < 0 || index >= int(self.layers().size())) throw py::index_error();
          clip::Image img;
          if (!self.layerImage(index, parseMode(mode), img))
            throw std::runtime_error("layer_image failed");
          return toBgra(img);
        },
        py::arg("index"), py::arg("mode") = "masked",
        "Pixels of one layer as BGRA bytes (layer.width*layer.height*4). "
        "Empty bytes for folders.")
    .def("layer_region",
        [](const clip::ClipFile &self, int index, int x, int y, int w, int h,
           const std::string &mode) {
          if (index < 0 || index >= int(self.layers().size())) throw py::index_error();
          clip::Rect r; r.x = x; r.y = y; r.w = w; r.h = h;
          clip::Image img;
          if (!self.layerRegion(index, r, parseMode(mode), img))
            throw std::runtime_error("layer_region failed");
          return toBgra(img);
        },
        py::arg("index"), py::arg("x"), py::arg("y"), py::arg("width"),
        py::arg("height"), py::arg("mode") = "masked",
        "Read only part of a layer as BGRA bytes. **Only the 256x256 blocks "
        "overlapping the rect are decompressed** — this is the one thing CLIP "
        "does that PSD cannot.")
    .def("merged_image",
        [](const clip::ClipFile &self) {
          clip::Image img;
          if (!self.mergedImage(img)) throw std::runtime_error("merged_image failed");
          return toBgra(img);
        },
        "Composite every layer bottom-to-top and return BGRA bytes. "
        "Matches what CLIP STUDIO stored in CanvasPreview.")
    .def("preview_png",
        [](const clip::ClipFile &self) -> py::object {
          std::vector<uint8_t> png; int w = 0, h = 0;
          if (!self.previewPng(png, w, h)) return py::none();
          return py::make_tuple(py::bytes((const char *)png.data(), png.size()), w, h);
        },
        "The preview CLIP STUDIO stored (CanvasPreview) as (png_bytes, w, h), "
        "or None. It is the finished artwork, but not always full size.")
    .def("attribute",
        [](const clip::ClipFile &self, int64_t offscreenId) -> py::object {
          const clip::OffscreenAttr *a = self.attribute(offscreenId);
          if (!a) return py::none();
          return py::cast(*a);
        },
        py::arg("offscreen_id"))
    .def("top_offscreen",
        [](const clip::ClipFile &self, int64_t layerMainId, bool mask) {
          return self.topOffscreen(layerMainId, mask);
        },
        py::arg("layer_main_id"), py::arg("mask") = false)
    .def("check",
        [](const clip::ClipFile &self) {
          std::string report;
          const bool ok = self.checkAll(&report);
          return py::make_tuple(ok, report);
        },
        "Walk every block and assert the structural invariants. "
        "Returns (ok, report).");

  // ---- 書く側 -------------------------------------------------------------
  //
  // tools/clip_write.py と同じことを C++ でやる。画素は **BGRA** で受ける
  // (読む側が BGRA を返すのに合わせてある)。

  py::class_<clip::ClipWriter>(m, "ClipWriter",
      "Writer for .clip files. Mirrors tools/clip_write.py.\n"
      "Rules CLIP STUDIO enforces but a tolerant reader cannot see:\n"
      " * Offscreen.BlockData is a BLOB, ExternalChunk.ExternalID is TEXT\n"
      " * BlockCheckSum must be 0 (a wrong non-zero value is rejected)\n"
      " * Mipmap.MipmapCount must equal the number of levels, or CSP crashes")
    .def(py::init<>())
    .def("load",
        [](clip::ClipWriter &self, const std::string &path) {
          if (!self.load(path.c_str()))
            throw std::runtime_error("load failed: " + self.error());
          return true;
        },
        py::arg("path"))
    .def("save",
        [](clip::ClipWriter &self, const std::string &path) {
          const int64_t n = self.save(path.c_str());
          if (!n) throw std::runtime_error("save failed: " + self.error());
          return n;
        },
        py::arg("path"),
        "Recompute every chunk offset, update ExternalChunk, serialize the "
        "database and write the file. Returns the byte count.")
    .def("set_layer_attr",
        [](clip::ClipWriter &self, int64_t mainId, py::object name,
           int64_t opacity, int64_t visible, int64_t composite, int64_t clipping,
           int64_t folder) {
          clip::ClipWriter::LayerAttr a;
          std::string keep;
          if (!name.is_none()) { keep = name.cast<std::string>(); a.name = keep.c_str(); }
          a.opacity = opacity; a.visibility = visible; a.composite = composite;
          a.clipping = clipping; a.folder = folder;
          if (!self.setLayerAttr(mainId, a))
            throw std::runtime_error(self.error());
          return true;
        },
        py::arg("main_id"), py::arg("name") = py::none(),
        py::arg("opacity") = -1, py::arg("visible") = -1,
        py::arg("composite") = -1, py::arg("clipping") = -1,
        py::arg("folder") = -1,
        "Layer attributes (opacity is 0..256, **not 255**).")
    .def("set_pixels",
        [](clip::ClipWriter &self, int64_t mainId, py::buffer bgra,
           uint32_t w, uint32_t h) {
          py::buffer_info info = bgra.request();
          if (size_t(info.size) * size_t(info.itemsize) != size_t(w) * h * 4)
            throw py::value_error("pixel buffer size mismatch");
          std::vector<uint8_t> rgba = bgraToRgba((const uint8_t *)info.ptr, w, h);
          if (!self.setPixels(mainId, rgba.data(), w, h))
            throw std::runtime_error(self.error());
          return true;
        },
        py::arg("main_id"), py::arg("bgra"), py::arg("width"), py::arg("height"),
        "Replace the 100% mipmap of a layer. Also drops the stale thumbnail.")
    .def("add_layer",
        [](clip::ClipWriter &self, int64_t copyFrom, const std::string &name,
           py::object bgra, uint32_t w, uint32_t h, int64_t after, int64_t parent) {
          std::vector<uint8_t> rgba;
          if (!bgra.is_none()) {
            py::buffer_info info = bgra.cast<py::buffer>().request();
            if (size_t(info.size) * size_t(info.itemsize) != size_t(w) * h * 4)
              throw py::value_error("pixel buffer size mismatch");
            rgba = bgraToRgba((const uint8_t *)info.ptr, w, h);
          }
          const int64_t id = self.addLayer(copyFrom, name,
                                           rgba.empty() ? nullptr : rgba.data(),
                                           w, h, after, parent);
          if (!id) throw std::runtime_error(self.error());
          return id;
        },
        py::arg("copy_from"), py::arg("name"), py::arg("bgra") = py::none(),
        py::arg("width") = 0, py::arg("height") = 0,
        py::arg("after") = -1, py::arg("parent") = 0,
        "Clone an existing layer as the template and add a new one. "
        "Returns the new Layer.MainId.")
    .def("delete_layer",
        [](clip::ClipWriter &self, int64_t mainId) {
          if (!self.deleteLayer(mainId)) throw std::runtime_error(self.error());
          return true;
        },
        py::arg("main_id"))
    .def("set_canvas_preview",
        [](clip::ClipWriter &self, py::buffer bgra, uint32_t w, uint32_t h) {
          py::buffer_info info = bgra.request();
          if (size_t(info.size) * size_t(info.itemsize) != size_t(w) * h * 4)
            throw py::value_error("pixel buffer size mismatch");
          std::vector<uint8_t> rgba = bgraToRgba((const uint8_t *)info.ptr, w, h);
          if (!self.setCanvasPreview(rgba.data(), w, h))
            throw std::runtime_error(self.error());
          return true;
        },
        py::arg("bgra"), py::arg("width"), py::arg("height"),
        "CLIP STUDIO shows this image the moment the file opens, so a stale "
        "one makes the canvas look wrong until a layer is touched.")
    .def("resize_canvas",
        [](clip::ClipWriter &self, uint32_t w, uint32_t h, double dpi) {
          if (!self.resizeCanvas(w, h, dpi)) throw std::runtime_error(self.error());
          return true;
        },
        py::arg("width"), py::arg("height"), py::arg("dpi") = 0.0,
        "Rebuild the canvas at a new size (mipmap chains grow or shrink). "
        "All block data is dropped; the caller must put it back.")
    .def("set_external_id_seed", &clip::ClipWriter::setExternalIdSeed,
        py::arg("seed"), "Fix the random seed for new external ids (tests).");

  m.def("validate",
      [](const std::string &path) {
        std::vector<std::string> problems;
        clip::validate(path.c_str(), problems);
        return problems;
      },
      py::arg("path"),
      "Check referential integrity and return the list of problems (empty is "
      "good). Run this before opening a written file in CLIP STUDIO.");
}
