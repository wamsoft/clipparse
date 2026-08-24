"""clip_doctor (レイヤ単位の診断と除去) のテスト。

`samples/` は gitignore されているので、ファイルが無ければ skip する。
壊し方は CSP 実機で確認済みの故障モード (STATUS.md ⑩⑪) に合わせてある。
"""
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
SAMPLES = os.path.join(ROOT, "samples")

import clip_doctor as doc                                   # noqa: E402
from clip_probe import walk_block_stream                    # noqa: E402
from clip_validate import validate                          # noqa: E402
from clip_write import ClipFile, as_str, _copy_row          # noqa: E402


def _sample(name):
    p = os.path.join(SAMPLES, name)
    if not os.path.exists(p):
        pytest.skip(f"sample not found: {name}")
    return p


def _render_chain(c, lid):
    """レイヤの描画ミップの (Mipmap ID, 先頭 MipmapInfo ID)。"""
    mm = c.db.execute("SELECT LayerRenderMipmap FROM Layer WHERE MainId=?",
                      (lid,)).fetchone()[0]
    base = c.db.execute("SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                        (mm,)).fetchone()[0]
    return mm, base


def test_clean_file_has_no_findings():
    c = ClipFile(_sample("opacity.clip"))
    d = doc.diagnose(c, deep=True)
    c.close()
    assert not d["fatal"]
    assert not d["removals"] and not d["unreachable"] and not d["repairs"]


def test_mipcount_mismatch_is_repaired(tmp_path):
    bad, out = str(tmp_path / "bad.clip"), str(tmp_path / "fixed.clip")
    c = ClipFile(_sample("opacity.clip"))
    mm, _ = _render_chain(c, 3)
    c.db.execute("UPDATE Mipmap SET MipmapCount=MipmapCount+1 WHERE MainId=?", (mm,))
    c.db.commit()
    c.save(bad)
    c.close()

    c = ClipFile(bad)
    d = doc.diagnose(c)
    assert ("mipcount", mm, 3) in d["repairs"]
    assert not d["removals"]                    # データ本体は無事なので除去しない
    doc.apply_fix(c, d)
    c.save(out)
    c.close()
    assert validate(out, verbose=False) == []
    c = ClipFile(out)
    assert doc.diagnose(c)["repairs"] == []
    c.close()


def test_broken_chain_layer_is_removed(tmp_path):
    bad, out = str(tmp_path / "bad.clip"), str(tmp_path / "fixed.clip")
    c = ClipFile(_sample("opacity.clip"))
    _, base = _render_chain(c, 3)
    c.db.execute("DELETE FROM MipmapInfo WHERE MainId=?", (base,))
    c.db.commit()
    c.save(bad)
    c.close()

    c = ClipFile(bad)
    d = doc.diagnose(c)
    assert d["removals"] == {3}
    doc.apply_fix(c, d)
    c.save(out)
    c.close()
    assert validate(out, verbose=False) == []
    c = ClipFile(out)
    assert c.db.execute("SELECT COUNT(*) FROM Layer WHERE MainId=3").fetchone()[0] == 0
    c.close()


def test_corrupt_zlib_needs_deep(tmp_path):
    bad = str(tmp_path / "bad.clip")
    c = ClipFile(_sample("opacity.clip"))
    _, base = _render_chain(c, 3)
    off = c.db.execute("SELECT Offscreen FROM MipmapInfo WHERE MainId=?",
                       (base,)).fetchone()[0]
    key = as_str(c.db.execute("SELECT BlockData FROM Offscreen WHERE MainId=?",
                              (off,)).fetchone()[0])
    for i, (e, p) in enumerate(c.externals):
        if as_str(e) != key:
            continue
        for rec in walk_block_stream(p, 0, len(p)):
            if rec["kind"] == "BlockDataBeginChunk" and rec["has_content"]:
                b = bytearray(p)
                q = rec["payload_offset"] + rec["compressed_size"] // 2
                for j in range(8):
                    b[q + j] ^= 0xFF
                c.externals[i] = (e, bytes(b))
                break
        break
    c.save(bad)
    c.close()

    c = ClipFile(bad)
    assert doc.diagnose(c, deep=False)["removals"] == set()   # 構造だけでは見えない
    assert doc.diagnose(c, deep=True)["removals"] == {3}
    c.close()


def test_orphan_row_is_purged_but_shared_entity_survives(tmp_path):
    bad, out = str(tmp_path / "bad.clip"), str(tmp_path / "fixed.clip")
    c = ClipFile(_sample("opacity.clip"))
    cur = c.db.cursor()
    # 実体を持つ行を複製して孤児にする。実体は元の行と共有される
    oid = cur.execute("SELECT MainId FROM Offscreen WHERE BlockData IS NOT NULL"
                      " ORDER BY MainId LIMIT 1").fetchone()[0]
    nid = cur.execute("SELECT MAX(MainId)+1 FROM Offscreen").fetchone()[0]
    _copy_row(cur, "Offscreen", "MainId", oid, {"MainId": nid, "LayerId": 88888})
    c.db.commit()
    c.save(bad)
    c.close()

    c = ClipFile(bad)
    n_ext = len(c.externals)
    d = doc.diagnose(c)
    assert ("orphans",) in d["repairs"]
    doc.apply_fix(c, d)
    assert len(c.externals) == n_ext            # 共有していた実体は残る
    c.save(out)
    c.close()
    assert validate(out, verbose=False) == []


def test_remove_folder_takes_subtree(tmp_path):
    out = str(tmp_path / "out.clip")
    c = ClipFile(_sample("folder.clip"))
    kid = c.db.execute("SELECT LayerFirstChildIndex FROM Layer WHERE MainId=6"
                       ).fetchone()[0]
    removed, _acts = doc.apply_fix(c, doc.diagnose(c), extra_remove=[6], auto=False)
    assert removed == {6, kid}
    c.save(out)
    c.close()
    assert validate(out, verbose=False) == []
    c = ClipFile(out)
    assert doc.diagnose(c, deep=True)["removals"] == set()
    c.close()


def test_cli_exit_codes(tmp_path):
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    tool = os.path.join(ROOT, "tools", "clip_doctor.py")
    assert subprocess.run([sys.executable, tool, _sample("opacity.clip")],
                          capture_output=True, env=env).returncode == 0

    bad = str(tmp_path / "bad.clip")
    c = ClipFile(_sample("opacity.clip"))
    mm, _ = _render_chain(c, 3)
    c.db.execute("UPDATE Mipmap SET MipmapCount=99 WHERE MainId=?", (mm,))
    c.db.commit()
    c.save(bad)
    c.close()
    assert subprocess.run([sys.executable, tool, bad],
                          capture_output=True, env=env).returncode == 1
