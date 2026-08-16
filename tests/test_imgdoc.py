"""psdparse 互換面 (tools/imgdoc.py) のテスト。

`samples/` は gitignore されているので、ファイルが無ければ skip する。

    python -m pytest tests -q
"""
import importlib.util
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
SAMPLES = os.path.join(ROOT, "samples")

psdparse = pytest.importorskip("psdparse")
np = pytest.importorskip("numpy")
import imgdoc                                            # noqa: E402


def _sample(name):
    p = os.path.join(SAMPLES, name)
    if not os.path.exists(p):
        pytest.skip(f"sample not found: {name}")
    return p


@pytest.fixture(scope="module")
def blend2():
    return imgdoc.open(_sample("blend2.clip"))


# --- psdparse と同じ形で読めるか -------------------------------------------

def test_header_surface(blend2):
    h = blend2.header
    assert (h.width, h.height) == (800, 1000)
    assert h.depth == 8
    assert h.mode == psdparse.COLOR_MODE_RGB


def test_layer_surface_matches_psdparse():
    """psdparse の examples/tools が触る属性がすべて生えていること。"""
    used = ["width", "height", "left", "top", "right", "bottom", "opacity",
            "fill_opacity", "visible", "layer_type", "name", "name_unicode",
            "blend_mode", "blend_mode_key", "clipping", "layer_id",
            "parent_index", "children", "is_group", "channels",
            "transparency_protected"]
    d = imgdoc.open(_sample("blend2.clip"))
    for attr in used:
        assert hasattr(d.layers[0], attr), attr
    for attr in ("header", "layers", "roots", "children", "merged_image",
                 "layer_image", "merged_alpha", "is_loaded"):
        assert hasattr(d, attr), attr


def test_enums_are_psdparse_enums(blend2):
    for l in blend2.layers:
        assert isinstance(l.layer_type, psdparse.LayerType)
        assert isinstance(l.blend_mode, psdparse.BlendMode)


def test_blend_modes_are_mapped(blend2):
    got = {l.blend_mode for l in blend2.layers}
    assert psdparse.BlendMode.SATURATION in got
    assert psdparse.BlendMode.VIVID_LIGHT in got
    assert psdparse.BlendMode.PASS_THROUGH in got       # 通過フォルダ


# --- ツリービュー ----------------------------------------------------------

def test_tree_covers_every_layer_once(blend2):
    seen = []

    def walk(i):
        seen.append(i)
        for c in blend2.layers[i].children:
            walk(c)

    for r in blend2.roots:
        walk(r)
    assert sorted(seen) == list(range(len(blend2.layers)))


def test_children_and_parent_agree(blend2):
    for i in range(len(blend2.layers)):
        for c in blend2.layers[i].children:
            assert blend2.layers[c].parent_index == i
    for r in blend2.roots:
        assert blend2.layers[r].parent_index == -1


def test_folders_are_groups_with_no_pixels(blend2):
    folders = [l for l in blend2.layers if l.is_group]
    assert folders
    for f in folders:
        assert f.layer_type == psdparse.LayerType.FOLDER
        assert (f.width, f.height) == (0, 0)     # psdparse のフォルダと同じ
        assert blend2.layer_image(blend2.layers.index(f)) == b""


def test_flat_order_is_bottom_to_top(blend2):
    """中身がフォルダより先に来る = PSD の平坦順と同じ。"""
    for i, l in enumerate(blend2.layers):
        for c in l.children:
            assert c < i


# --- 画素 ------------------------------------------------------------------

def test_layer_image_size(blend2):
    for i, l in enumerate(blend2.layers):
        data = blend2.layer_image(i)
        assert len(data) == l.width * l.height * 4


def test_layer_image_modes(blend2):
    with pytest.raises(ValueError):
        blend2.layer_image(0, "bogus")
    with pytest.raises(IndexError):
        blend2.layer_image(999)


def test_merged_image_matches_canvas_preview():
    """CSP が保存した完成画と一致すること (合成の正解合わせ)。"""
    import io
    from PIL import Image
    d = imgdoc.open(_sample("text.clip"))
    blob = d.clip.cur.execute("SELECT ImageData FROM CanvasPreview").fetchone()[0]
    ref = np.array(Image.open(io.BytesIO(blob)).convert("RGB")).astype(int)
    W, H = d.header.width, d.header.height
    got = np.frombuffer(d.merged_image(), np.uint8).reshape(H, W, 4)[..., [2, 1, 0]]
    assert np.abs(got.astype(int) - ref).max() == 0


def test_text_layer_uses_object_bbox():
    """テキストは外接矩形の Offscreen を配置位置に置く (キャンバス全面ではない)。"""
    d = imgdoc.open(_sample("text.clip"))
    texts = [l for l in d.layers if l.layer_type == psdparse.LayerType.TEXT]
    assert texts
    for t in texts:
        assert 0 < t.width < d.header.width
        assert (t.left, t.top) != (0, 0)


def test_grayscale_and_monochrome_read_as_rgba():
    for name in ("gray_drawn.clip", "mono_drawin.clip"):
        d = imgdoc.open(_sample(name))
        i = len(d.layers) - 1
        assert len(d.layer_image(i)) == d.layers[i].width * d.layers[i].height * 4


# --- psd の側も同じ口で開けるか --------------------------------------------

def test_open_dispatches_on_extension(tmp_path):
    d = imgdoc.open(_sample("test000.clip"))
    assert isinstance(d, imgdoc.ClipDocument)

    # 変換して PSD を作り、同じ open() で読めることを確かめる
    psd_path = tmp_path / "out.psd"
    p = psdparse.PSDFile()
    p.create_blank(8, 8)
    p.add_layer("x", 0, 0, bytes([0, 0, 0, 255]) * 64, 8, 8)
    assert p.save(str(psd_path))
    q = imgdoc.open(str(psd_path))
    assert isinstance(q, psdparse.PSDFile)
    assert q.header.width == 8
