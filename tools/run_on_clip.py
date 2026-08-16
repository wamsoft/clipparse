"""psdparse 向けのスクリプトを、そのまま .clip に対して走らせるための実行器。

    python tools/run_on_clip.py <script.py> [args...]

`imgdoc.patch_psdparse()` で `psdparse.PSDFile` を差し替えてから対象を実行する。
psdparse の examples/tools が**無改造で .clip に通るか**を確かめるのが目的。
"""
import os
import runpy
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import imgdoc

if len(sys.argv) < 2:
    print(__doc__)
    raise SystemExit(2)

script = os.path.abspath(sys.argv[1])
sys.path.insert(0, os.path.dirname(script))     # 例: examples/ 内の相互 import 用
imgdoc.patch_psdparse()
sys.argv = sys.argv[1:]
runpy.run_path(script, run_name="__main__")
