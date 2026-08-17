"""書き込み経路 (W1〜W4) のテスト。

`samples/` は gitignore されているので、ファイルが無ければ skip する。

    python -m pytest tests -q
"""
import hashlib
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
SAMPLES = os.path.join(ROOT, "samples")

np = pytest.importorskip("numpy")
pytest.importorskip("PIL")

import clip_build as cb                                    # noqa: E402
import clip_encode as enc                                  # noqa: E402
from clip_write import ClipFile, add_layer, delete_layer    # noqa: E402


def _sample(name):
    p = os.path.join(SAMPLES, name)
    if not os.path.exists(p):
        pytest.skip(f"sample not found: {name}")
    return p


# --- ブロックのエンコード ---------------------------------------------------

def test_block_encode_decode_roundtrip():
    rng = np.random.default_rng(0)
    rgba = rng.integers(0, 256, (256, 256, 4), dtype=np.uint8)
    buf = enc.encode_rgba_block(rgba, 256, 256)
    assert len(buf) == (256 + 64) * 256 * 4
    assert np.array_equal(enc.decode_rgba_block(buf, 256, 256), rgba)


def test_empty_block_record_is_104_bytes():
    assert len(enc.build_block_record(0, None, 256, 256)) == enc.EMPTY_RECORD_SIZE


def test_data_block_record_is_clen_plus_112():
    raw = enc.encode_rgba_block(np.zeros((256, 256, 4), np.uint8), 256, 256)
    rec = enc.build_block_record(3, raw, 256, 256)
    import struct
    import zlib
    assert struct.unpack_from(">I", rec, 0)[0] == len(rec)
    assert len(rec) == len(zlib.compress(raw, 9)) + 112


# --- Attribute の作り替え ---------------------------------------------------

def test_mip_levels_match_measured_files():
    """実ファイルで観測した段数と寸法 (docs/CLIP_FORMAT.md)。"""
    assert cb.mip_levels(1400, 700) == [(1400, 700), (700, 350), (350, 175),
                                        (175, 87), (87, 43)]
    assert cb.mip_levels(800, 1000) == [(800, 1000), (400, 500), (200, 250),
                                        (100, 125)]
    assert cb.mip_levels(300, 400) == [(300, 400), (150, 200), (75, 100)]


def test_retarget_attribute_is_identity():
    """同じ寸法・同じブロック列で作り直すと**バイト一致**すること。"""
    c = ClipFile(_sample("blend2.clip"))
    cur = c.db.cursor()
    n = 0
    for attr, in cur.execute("SELECT Attribute FROM Offscreen"):
        if attr is None:
            continue
        attr = bytes(attr)
        w, h, _co, _ro = cb.attribute_dims(attr)
        sizes = enc.parse_attr(attr)["block_sizes"]
        assert cb.retarget_attribute(attr, w, h, sizes) == attr
        n += 1
    c.close()
    assert n > 10


# --- ファイル単位 -----------------------------------------------------------

def test_roundtrip_is_byte_identical(tmp_path):
    src = _sample("opacity.clip")
    dst = str(tmp_path / "out.clip")
    c = ClipFile(src)
    c.save(dst)
    c.close()
    a = hashlib.sha256(open(src, "rb").read()).hexdigest()
    b = hashlib.sha256(open(dst, "rb").read()).hexdigest()
    assert a == b


def test_add_then_delete_layer_restores_tree(tmp_path):
    c = ClipFile(_sample("opacity.clip"))
    cur = c.db.cursor()
    before = [r[0] for r in cur.execute("SELECT MainId FROM Layer ORDER BY MainId")]
    new = add_layer(c, 3, "テスト")
    assert new not in before
    delete_layer(c, new)
    after = [r[0] for r in cur.execute("SELECT MainId FROM Layer ORDER BY MainId")]
    assert after == before
    dst = str(tmp_path / "out.clip")
    c.save(dst)
    c.close()
    assert os.path.getsize(dst) > 0


def test_setpixels_reads_back_exactly(tmp_path):
    imgdoc = pytest.importorskip("imgdoc")
    src = _sample("opacity.clip")
    dst = str(tmp_path / "px.clip")
    rng = np.random.default_rng(1)
    rgba = rng.integers(0, 256, (400, 300, 4), dtype=np.uint8)

    c = ClipFile(src)
    new = add_layer(c, 3, "画素テスト", rgba)
    c.save(dst)
    c.close()

    d = imgdoc.open(dst)
    i = [k for k, l in enumerate(d.layers) if l.main_id == new][0]
    got = np.frombuffer(d.layer_image(i), np.uint8).reshape(400, 300, 4)
    assert np.array_equal(got[..., [2, 1, 0, 3]], rgba)


# --- PSD -> CLIP (W4) -------------------------------------------------------

@pytest.mark.parametrize("name", ["folder.clip", "blend2.clip", "test000.clip"])
def test_psd_roundtrip_keeps_pixels_and_tree(tmp_path, name):
    """CLIP -> PSD -> CLIP で合成結果が**バイト一致**すること。"""
    pytest.importorskip("psdparse")
    imgdoc = pytest.importorskip("imgdoc")
    src = _sample(name)
    psd = str(tmp_path / "mid.psd")
    back = str(tmp_path / "back.clip")

    for cmd in ([sys.executable, os.path.join(ROOT, "tools", "clip_to_psd.py"),
                 src, psd],
                [sys.executable, os.path.join(ROOT, "tools", "psd_to_clip.py"),
                 psd, back, "--verify"]):
        r = subprocess.run(cmd, capture_output=True)
        assert r.returncode == 0, r.stderr.decode("utf-8", "replace")

    a, b = imgdoc.open(src), imgdoc.open(back)
    assert (a.header.width, a.header.height) == (b.header.width, b.header.height)
    H, W = a.header.height, a.header.width
    ia = np.frombuffer(a.merged_image(), np.uint8).reshape(H, W, 4).astype(int)
    ib = np.frombuffer(b.merged_image(), np.uint8).reshape(H, W, 4).astype(int)
    vis = (ia[..., 3] > 0) | (ib[..., 3] > 0)      # α=0 の RGB は意味を持たない
    d = np.abs(ia - ib)
    d[..., :3] *= vis[..., None]
    assert d.max() == 0


def test_psd_to_clip_opacity_is_reversible():
    """CLIP 0..256 と PSD 0..255 の往復 (c=1 以外は戻る)。"""
    from psd_to_clip import PSD_TO_BLEND       # noqa: F401  (import 可能なこと)
    for c in range(257):
        p = c * 255 // 256                     # clip_to_psd と同じ切り捨て
        back = min(256, -(-p * 256 // 255))    # psd_to_clip と同じ切り上げ
        assert back == c or c == 1


# --- 格納型 (CSP が実体を見つけられるか) ------------------------------------

def _typeof(path):
    c = ClipFile(path)
    cur = c.db.cursor()
    a = cur.execute("SELECT DISTINCT typeof(BlockData) FROM Offscreen").fetchall()
    b = cur.execute("SELECT DISTINCT typeof(ExternalID) FROM ExternalChunk").fetchall()
    c.close()
    return sorted(x[0] for x in a), sorted(x[0] for x in b)


def test_samples_use_blob_blockdata_and_text_externalid():
    """CSP が書く格納型 [実測: 33 ファイル 7,000 行超で例外なし]。"""
    for name in ("opacity.clip", "blend2.clip", "text.clip", "test000.clip"):
        assert _typeof(_sample(name)) == (["blob"], ["text"]), name


def test_added_layer_keeps_csp_storage_classes(tmp_path):
    """**同じ 40 文字の ID なのに格納型が逆**。取り違えると CSP は実体を
    見つけられず、そのレイヤを全面透明として開く (こちらのリーダは気付けない)。
    """
    dst = str(tmp_path / "added.clip")
    c = ClipFile(_sample("opacity.clip"))
    add_layer(c, 3, "格納型テスト", np.zeros((400, 300, 4), np.uint8))
    c.save(dst)
    c.close()
    assert _typeof(dst) == (["blob"], ["text"])


def test_psd_to_clip_keeps_csp_storage_classes(tmp_path):
    pytest.importorskip("psdparse")
    psd = str(tmp_path / "mid.psd")
    clip = str(tmp_path / "out.clip")
    for cmd in ([sys.executable, os.path.join(ROOT, "tools", "clip_to_psd.py"),
                 _sample("folder.clip"), psd],
                [sys.executable, os.path.join(ROOT, "tools", "psd_to_clip.py"),
                 psd, clip]):
        r = subprocess.run(cmd, capture_output=True)
        assert r.returncode == 0, r.stderr.decode("utf-8", "replace")
    assert _typeof(clip) == (["blob"], ["text"])


# --- 参照整合性 -------------------------------------------------------------

def test_real_files_pass_validation():
    import clip_validate
    for name in ("opacity.clip", "folder.clip", "blend2.clip", "test000.clip",
                 "addlayer_csp.clip"):
        assert clip_validate.validate(_sample(name), verbose=False) == [], name


def test_written_files_pass_validation(tmp_path):
    import clip_validate
    dst = str(tmp_path / "out.clip")
    c = ClipFile(_sample("opacity.clip"))
    add_layer(c, 3, "検査テスト", np.zeros((400, 300, 4), np.uint8))
    c.save(dst)
    c.close()
    assert clip_validate.validate(dst, verbose=False) == []


def test_resize_keeps_mipmap_count_in_sync(tmp_path):
    """**`Mipmap.MipmapCount` が段数と食い違うと CSP が読み込み中に落ちる**
    [実測: 段数を減らしたファイルだけ落ちた]。こちらのリーダは NextIndex=0 で
    止まるので気付けない。
    """
    dst = str(tmp_path / "resized.clip")
    c = ClipFile(_sample("emptyimage.clip"))          # 1600x1200 = 5 段
    levels = cb.resize_canvas(c.db, 300, 400)         # 3 段へ縮める
    assert len(levels) == 3
    c.externals = []
    c.save(dst)
    c.close()

    c = ClipFile(dst)
    cur = c.db.cursor()
    for mm, base, count in cur.execute(
            "SELECT MainId, BaseMipmapInfo, MipmapCount FROM Mipmap"):
        n, node = 0, base
        while node:
            node = cur.execute("SELECT NextIndex FROM MipmapInfo WHERE MainId=?",
                               (node,)).fetchone()[0]
            n += 1
        assert count == n == 3, mm
    c.close()
