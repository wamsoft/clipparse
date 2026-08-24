"""clip_export (PNG 書き出し) のテスト。

PNG ライタ自体は純 Python なので常に検証できる。書き出した画素の正しさは
C++ 拡張が要るので、無い環境では skip (`tools/imgdoc.py` と同じ判定)。
"""
import json
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
SAMPLES = os.path.join(ROOT, "samples")

np = pytest.importorskip("numpy")
Image = pytest.importorskip("PIL.Image")

import clip_export as ex                                    # noqa: E402


def _sample(name):
    p = os.path.join(SAMPLES, name)
    if not os.path.exists(p):
        pytest.skip(f"sample not found: {name}")
    return p


def _need_cpp():
    try:
        import clipparse
    except ImportError:
        clipparse = None
    if clipparse is None or not hasattr(clipparse, "ClipFile"):
        pytest.skip("clipparse C++ extension not importable")


def test_png_writer_roundtrip(tmp_path):
    rng = np.random.default_rng(0)
    bgra = rng.integers(0, 256, (37, 61, 4), dtype=np.uint8)
    p = str(tmp_path / "t.png")
    ex.write_png(p, bgra.tobytes(), 61, 37)
    back = np.array(Image.open(p).convert("RGBA"))
    assert np.array_equal(back, bgra[..., [2, 1, 0, 3]])


def test_merged_matches_reference(tmp_path):
    _need_cpp()
    import imgdoc
    src = _sample("opacity.clip")
    out = str(tmp_path / "m.png")
    assert ex.main([src, "-o", out]) == 0
    d = imgdoc.open(src)
    w, h = d.header.width, d.header.height
    ref = np.frombuffer(d.merged_image(), np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]
    assert np.array_equal(np.array(Image.open(out).convert("RGBA")), ref)


def test_layers_export_with_manifest(tmp_path):
    _need_cpp()
    import imgdoc
    src = _sample("opacity.clip")
    out = str(tmp_path / "layers")
    assert ex.main([src, "--layers", out]) == 0
    m = json.load(open(os.path.join(out, "manifest.json"), encoding="utf-8"))
    assert m["canvas"] == {"width": 300, "height": 400}
    assert len(m["layers"]) == 3
    d = imgdoc.open(src)
    for e in m["layers"]:
        li = d.layers[e["index"]]
        ref = np.frombuffer(d.layer_image(e["index"]), np.uint8)
        ref = ref.reshape(li.height, li.width, 4)[..., [2, 1, 0, 3]]
        png = np.array(Image.open(os.path.join(out, e["file"])).convert("RGBA"))
        assert np.array_equal(png, ref), e["name"]


def test_text_layer_bbox_export(tmp_path):
    _need_cpp()
    src = _sample("text.clip")
    out = str(tmp_path / "layers")
    assert ex.main([src, "--layers", out]) == 0
    m = json.load(open(os.path.join(out, "manifest.json"), encoding="utf-8"))
    texts = [e for e in m["layers"] if e["width"] < 300]
    assert texts, "テキストレイヤが外接矩形で出ているはず"


def test_list_and_single_layer(tmp_path):
    _need_cpp()
    src = _sample("opacity.clip")
    assert ex.main([src, "--list"]) == 0
    out = str(tmp_path / "one.png")
    assert ex.main([src, "--layer", "1", "-o", out]) == 0
    assert os.path.exists(out)
    assert ex.main([src, "--layer", "99", "-o", out]) == 1
