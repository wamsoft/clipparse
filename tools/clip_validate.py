"""`.clip` の参照整合性を検査する。

    python tools/clip_validate.py a.clip b.clip ...

`clip_cli --check` はブロックの読み出しを検査するが、こちらは
**SQLite の中の参照が全部生きているか**を見る。CSP が落ちるファイルと
開けるファイルの差を機械的に洗い出すために書いた。

見るもの:

- `Canvas` が指すレイヤ (ルート / カレント) が実在するか
- レイヤの兄弟・子チェーンに**閉路**が無いか、全レイヤに到達できるか
- `LayerRenderMipmap` → `Mipmap` → `MipmapInfo` 連鎖 → `Offscreen` が全部生きているか
- `Mipmap.MipmapCount` が実際の段数と一致するか (**違うと CSP が落ちる**)
- `LayerThumbnail.ThumbnailOffscreen` が生きているか
- `LayerId` が実在しない**孤児行**が無いか
- `Offscreen.BlockData` の格納型 (**BLOB でないと CSP が実体を解決できない**)
- `ExternalChunk` の行と実チャンクの数が合うか
  (どこからも参照されていないチャンクは CSP のファイルにもあるので情報扱い)

依存: 標準ライブラリのみ。
"""

import sys

try:                                        # pip 版 (clipparse_tools パッケージ内)
    from .clip_write import ClipFile, as_str
except ImportError:                         # リポジトリの tools/ から直接実行
    from clip_write import ClipFile, as_str


def _ids(cur, table, col="MainId"):
    return {r[0] for r in cur.execute(f"SELECT [{col}] FROM [{table}]")}


def validate(path, verbose=True):
    c = ClipFile(path)
    cur = c.db.cursor()
    bad = []

    layers = _ids(cur, "Layer")
    mipmaps = _ids(cur, "Mipmap")
    infos = _ids(cur, "MipmapInfo")
    offs = _ids(cur, "Offscreen")
    thumbs = _ids(cur, "LayerThumbnail")

    canvas_id, root, cur_layer = cur.execute(
        "SELECT MainId, CanvasRootFolder, CanvasCurrentLayer FROM Canvas").fetchone()
    if root not in layers:
        bad.append(f"Canvas.CanvasRootFolder={root} が Layer に無い")
    if cur_layer and cur_layer not in layers:
        bad.append(f"Canvas.CanvasCurrentLayer={cur_layer} が Layer に無い"
                   f" (削除したレイヤを指したまま)")

    # --- ツリー: 閉路と到達性 ---
    seen = set()

    def walk(node, depth=0):
        while node:
            if node in seen:
                bad.append(f"レイヤの兄弟チェーンに閉路: #{node}")
                return
            if node not in layers:
                bad.append(f"存在しないレイヤ #{node} をチェーンが指している")
                return
            seen.add(node)
            kid, nxt = cur.execute(
                "SELECT LayerFirstChildIndex, LayerNextIndex FROM Layer"
                " WHERE MainId=?", (node,)).fetchone()
            if kid:
                walk(kid, depth + 1)
            node = nxt

    if root in layers:
        seen.add(root)
        kid = cur.execute("SELECT LayerFirstChildIndex FROM Layer WHERE MainId=?",
                          (root,)).fetchone()[0]
        walk(kid)
    lost = layers - seen
    if lost:
        bad.append(f"ツリーから到達できないレイヤ: {sorted(lost)}")

    # --- ミップ連鎖 ---
    for lid, mm, tn in cur.execute(
            "SELECT MainId, LayerRenderMipmap, LayerRenderThumbnail FROM Layer"):
        if mm and mm not in mipmaps:
            bad.append(f"Layer #{lid}.LayerRenderMipmap={mm} が Mipmap に無い")
        elif mm:
            node, count = cur.execute(
                "SELECT BaseMipmapInfo, MipmapCount FROM Mipmap WHERE MainId=?",
                (mm,)).fetchone()
            hops = 0
            while node:
                if node not in infos:
                    bad.append(f"Layer #{lid} のミップ連鎖が MipmapInfo #{node} で切れる")
                    break
                o, nxt = cur.execute(
                    "SELECT Offscreen, NextIndex FROM MipmapInfo WHERE MainId=?",
                    (node,)).fetchone()
                if o not in offs:
                    bad.append(f"MipmapInfo #{node}.Offscreen={o} が Offscreen に無い")
                node = nxt
                hops += 1
                if hops > 64:
                    bad.append(f"Layer #{lid} のミップ連鎖が終わらない (閉路)")
                    break
            if count != hops:
                bad.append(f"Mipmap #{mm}.MipmapCount={count} が実際の段数 {hops} と違う"
                           f" (CSP が存在しない段まで辿って落ちる)")
        if tn and tn not in thumbs:
            bad.append(f"Layer #{lid}.LayerRenderThumbnail={tn} が LayerThumbnail に無い")
        elif tn:
            o = cur.execute("SELECT ThumbnailOffscreen FROM LayerThumbnail"
                            " WHERE MainId=?", (tn,)).fetchone()[0]
            if o not in offs:
                bad.append(f"LayerThumbnail #{tn}.ThumbnailOffscreen={o} が無い")

    # --- 孤児 ---
    for table in ("Offscreen", "Mipmap", "MipmapInfo", "LayerThumbnail"):
        orphan = sorted({r[0] for r in cur.execute(
            f"SELECT MainId FROM [{table}] WHERE LayerId NOT IN"
            f" (SELECT MainId FROM Layer)")})
        if orphan:
            bad.append(f"{table} に孤児行 (LayerId が Layer に無い): {orphan[:12]}"
                       f"{' ...' if len(orphan) > 12 else ''} 計 {len(orphan)}")

    # --- 格納型 ---
    t = [r[0] for r in cur.execute("SELECT DISTINCT typeof(BlockData) FROM Offscreen")]
    if sorted(t) != ["blob"]:
        bad.append(f"Offscreen.BlockData の格納型が {t} (CSP は blob のみ)")
    t = [r[0] for r in cur.execute(
        "SELECT DISTINCT typeof(ExternalID) FROM ExternalChunk")]
    if t and sorted(t) != ["text"]:
        bad.append(f"ExternalChunk.ExternalID の格納型が {t} (CSP は text のみ)")

    # --- 実体 ---
    have = {as_str(e) for e, in cur.execute("SELECT ExternalID FROM ExternalChunk")}
    # 参照元は `Offscreen.BlockData` だけではない。`ExternalTableAndColumnName` が
    # 「この列に external_id が入る」を宣言しているので、そこを全部見る
    # (実運用ファイルでは Track / TimeLapseBlob / Canvas3DModelBank などが持つ)。
    used = set()
    for table, col in cur.execute(
            "SELECT TableName, ColumnName FROM ExternalTableAndColumnName"):
        try:
            used |= {as_str(b) for b, in cur.execute(
                f"SELECT [{col}] FROM [{table}] WHERE [{col}] IS NOT NULL")}
        except Exception:
            bad.append(f"ExternalTableAndColumnName の {table}.{col} が引けない")
    # **未参照のチャンクは異常ではない**。CSP 自身のファイルにも残っている
    # (haruse-ja に 2 件、nazoani01 に 52 件。`RemovedExternal` は空)。
    # 編集で捨てられた実体を回収していないだけなので、情報として出すに留める。
    note = (f"どの列からも指されていない ExternalChunk が {len(have - used)} 件 "
            f"(CSP のファイルにもあるので異常ではない)") if have - used else None
    chunk_ids = {as_str(e) for e, _p in c.externals}
    if chunk_ids != have:
        bad.append(f"ExternalChunk の行 {len(have)} 件と実チャンク {len(chunk_ids)} 件が食い違う")

    if verbose:
        print(f"{path}")
        print(f"  レイヤ {len(layers)} / Offscreen {len(offs)} / 実体 {len(chunk_ids)}")
        if note:
            print(f"  -- {note}")
        for b in bad:
            print(f"  NG {b}")
        if not bad:
            print("  => 参照整合性 OK")
    c.close()
    return bad


def main(argv=None):
    argv = argv or sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    return 1 if [p for p in argv if validate(p)] else 0


if __name__ == "__main__":
    sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
    sys.exit(main())
