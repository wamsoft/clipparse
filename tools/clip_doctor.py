"""`.clip` のレイヤ単位の健全性診断と、不正な部分の除去。

    python tools/clip_doctor.py IN.clip                        # 診断 (ツリー + 問題一覧)
    python tools/clip_doctor.py IN.clip --deep                 # 全ブロックを zlib 展開して照合
    python tools/clip_doctor.py IN.clip --fix --out OUT.clip   # 修復 + 壊れたレイヤの除去
    python tools/clip_doctor.py IN.clip --remove 7 --out OUT.clip

`clip_validate.py` がファイル全体の合否を出すのに対し、こちらは
**どのレイヤが悪いか**まで切り分けて、レイヤ単位で取り除けるようにする。

判定の分類:

- **除去候補**: レイヤの描画データそのものが壊れている (描画ミップ連鎖の断線 /
  Attribute の破損 / ブロック列の破損 / 実体チャンクの欠落)。修復のしようが
  ないので --fix はレイヤごと取り除く。
- **修復可能**: 参照や数値の食い違いで、描画データ本体は無事
  (`MipmapCount` 不一致 / 死んだリンク / 格納型 / 不透明度の範囲逸脱 /
  マスク・サムネイルだけの破損 / 孤児行 / ExternalChunk の食い違い)。
  --fix がその場で直す。マスク・サムネイルは壊れた部分だけ落とす。
  行ごと消してポインタを 0 にする手術は CSP 5.0.4 実機で確認済み
  [実測: DOCTOR_TEST 2026-08-24。レイヤ除去 / マスク外し / サムネイル再生成とも OK]。
- **情報**: CSP のファイルにもある無害な状態 (未参照の実体チャンクなど)。

--fix の後は `clip_validate.py` と同じ検査を自動で通す。CSP で開く前提の
確認はそちらに任せる。

依存: 標準ライブラリのみ (CanvasPreview の再合成だけ numpy + imgdoc があれば行う)。
"""

import argparse
import sys
import zlib

try:                                        # pip 版 (clipparse_tools パッケージ内)
    from .clip_probe import parse_attribute, walk_block_stream, walk_chunks
    from .clip_write import ClipFile, as_str
except ImportError:                         # リポジトリの tools/ から直接実行
    from clip_probe import parse_attribute, walk_block_stream, walk_chunks
    from clip_write import ClipFile, as_str

CHAIN_LIMIT = 64          # ミップ連鎖の暴走ガード (実ファイルは高々 10 段)


def _ids(cur, table, col="MainId"):
    return {r[0] for r in cur.execute(f"SELECT [{col}] FROM [{table}]")}


def _table_cols(cur, table):
    return {r[1] for r in cur.execute(f"PRAGMA table_info([{table}])")}


def _extmap(cur):
    """テーブル → external_id を持つ列の一覧 (ExternalTableAndColumnName)。"""
    out = {}
    for t, col in cur.execute(
            "SELECT TableName, ColumnName FROM ExternalTableAndColumnName"):
        out.setdefault(t, []).append(col)
    return out


def _used_external_ids(cur):
    """どこかの列から参照されている external_id の集合。"""
    used = set()
    tables = {r[0] for r in cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    for table, cols in _extmap(cur).items():
        if table not in tables:
            continue
        have = _table_cols(cur, table)
        for col in cols:
            if col in have:
                used |= {as_str(v) for v, in cur.execute(
                    f"SELECT [{col}] FROM [{table}] WHERE [{col}] IS NOT NULL")}
    return used


# --------------------------------------------------------------------------
# 診断
# --------------------------------------------------------------------------

def diagnose(c, deep=False):
    """検査して結果を dict で返す。

    layers        ツリー順の表示用レコード
    unreachable   ツリーから到達できないレイヤ ID
    layer_issues  {layer_id: [(sev, msg)]}  sev は remove / repair / info
    removals      除去候補のレイヤ ID
    repairs       修復アクション (tag, ...) のリスト
    global        ファイル単位の問題 [(sev, msg)]
    fatal         レイヤ手術では直せない問題 (SQLite 破損など)
    """
    cur = c.db.cursor()
    d = {"layers": [], "unreachable": [], "layer_issues": {}, "removals": set(),
         "repairs": [], "global": [], "fatal": None, "stats": {}}

    def glob(sev, msg):
        d["global"].append((sev, msg))

    def lay_issue(lid, sev, msg):
        d["layer_issues"].setdefault(lid, []).append((sev, msg))
        if sev == "remove":
            d["removals"].add(lid)

    ok = cur.execute("PRAGMA quick_check(1)").fetchone()[0]
    if ok != "ok":
        d["fatal"] = f"SQLite 領域そのものが壊れている: {ok}"
        return d

    layers = {}
    want = ["MainId", "LayerName", "LayerType", "LayerFolder", "LayerVisibility",
            "LayerComposite", "LayerOpacity", "LayerFirstChildIndex",
            "LayerNextIndex", "LayerRenderMipmap", "LayerRenderThumbnail",
            "LayerLayerMaskMipmap", "LayerLayerMaskThumbnail"]
    have_cols = _table_cols(cur, "Layer")
    sel = [w if w in have_cols else "0" for w in want]   # 旧版で列が無ければ 0 扱い
    for r in cur.execute(f"SELECT {', '.join(sel)} FROM Layer"):
        layers[r[0]] = dict(zip(want, r))

    mipmaps = _ids(cur, "Mipmap")
    infos = _ids(cur, "MipmapInfo")
    thumbs = _ids(cur, "LayerThumbnail")
    ext_rows = {as_str(e) for e, in cur.execute("SELECT ExternalID FROM ExternalChunk")}
    exts = {as_str(e): p for e, p in c.externals}
    d["stats"] = {"layers": len(layers),
                  "offscreens": cur.execute("SELECT COUNT(*) FROM Offscreen").fetchone()[0],
                  "chunks": len(exts)}

    root, cur_layer = cur.execute(
        "SELECT CanvasRootFolder, CanvasCurrentLayer FROM Canvas").fetchone()
    if root not in layers:
        d["fatal"] = f"Canvas.CanvasRootFolder={root} が Layer に無い (レイヤ手術では直せない)"
        return d
    if cur_layer and cur_layer not in layers:
        glob("repair", f"Canvas.CanvasCurrentLayer={cur_layer} が消えたレイヤを指している")
        d["repairs"].append(("curlayer",))

    # --- ツリー: 到達性・閉路・死んだリンク --------------------------------
    seen = set()
    order = []                                    # [(id, depth)]

    def walk(first, holder, hcol, depth):
        node = first
        h, hc = holder, hcol
        while node:
            if node in seen:
                glob("repair", f"レイヤ連結に閉路: #{h}.{hc} が #{node} へ戻る")
                d["repairs"].append(("deadlink", h, hc))
                return
            if node not in layers:
                glob("repair", f"存在しないレイヤ #{node} を #{h}.{hc} が指している")
                d["repairs"].append(("deadlink", h, hc))
                return
            seen.add(node)
            order.append((node, depth))
            r = layers[node]
            if r["LayerFirstChildIndex"]:
                walk(r["LayerFirstChildIndex"], node, "LayerFirstChildIndex", depth + 1)
            h, hc = node, "LayerNextIndex"
            node = r["LayerNextIndex"]

    seen.add(root)
    order.append((root, 0))
    if layers[root]["LayerFirstChildIndex"]:
        walk(layers[root]["LayerFirstChildIndex"], root, "LayerFirstChildIndex", 1)
    d["unreachable"] = sorted(set(layers) - seen)

    # --- Offscreen 1 枚の検査 ---------------------------------------------
    def check_offscreen(lid, oid, sev, label):
        """構造検査。返り値は ({total, data}, 問題があったか)。"""
        st = {"total": 0, "data": 0}
        row = cur.execute("SELECT Attribute, BlockData FROM Offscreen WHERE MainId=?",
                          (oid,)).fetchone()
        if row is None:
            lay_issue(lid, sev, f"{label}: Offscreen #{oid} が無い")
            return st, True
        attr, bd = row
        if attr is None:                          # 実ファイルに NULL の行がある。触らない
            return st, False
        try:
            a = parse_attribute(bytes(attr))
        except Exception as e:
            lay_issue(lid, sev, f"{label}: Offscreen #{oid} の Attribute が壊れている ({e})")
            return st, True
        sizes = a["block_sizes"]
        st["total"] = len(sizes)
        if len(sizes) != a["cols"] * a["rows"]:
            lay_issue(lid, sev, f"{label}: BlockSize の要素数 {len(sizes)} が"
                                f"グリッド {a['cols']}x{a['rows']} と合わない")
            return st, True
        if bd is None:
            return st, False
        key = as_str(bd)
        if key not in ext_rows:                   # 実体なし = 空。BlockSize に値が
            return st, False                      # 入っていても異常ではない
        payload = exts.get(key)
        if payload is None:
            lay_issue(lid, sev, f"{label}: 実体 {key[:12]}… が ExternalChunk に"
                                f"載っているのにファイル内に無い")
            return st, True

        err = None
        tails = {}
        n = 0
        prev_p = -1
        try:
            for rec in walk_block_stream(payload, 0, len(payload)):
                if rec["rel_offset"] == prev_p:
                    err = "サブレコードが前へ進まない"
                    break
                prev_p = rec["rel_offset"]
                if rec["kind"] == "BlockDataBeginChunk":
                    bi = rec["block_index"]
                    if bi != n:
                        err = f"ブロック番号が飛ぶ (#{n} の位置に {bi})"
                        break
                    if rec["record_size"] != sizes[bi]:
                        err = (f"ブロック {bi} の実サイズ {rec['record_size']} が"
                               f" BlockSize[{bi}]={sizes[bi]} と違う")
                        break
                    if rec["has_content"]:
                        st["data"] += 1
                        if deep:
                            z = payload[rec["payload_offset"]:
                                        rec["payload_offset"] + rec["compressed_size"]]
                            try:
                                dec = zlib.decompress(z)
                            except zlib.error as e:
                                err = f"ブロック {bi} の zlib が壊れている ({e})"
                                break
                            if len(dec) != rec["decompressed_size"]:
                                err = (f"ブロック {bi} の展開サイズ {len(dec)} が"
                                       f"宣言 {rec['decompressed_size']} と違う")
                                break
                    n += 1
                elif rec["kind"] in ("BlockStatus", "BlockCheckSum"):
                    tails[rec["kind"]] = rec["count"]
                else:
                    err = f"未知のサブレコード {rec.get('name')!r}"
                    break
            else:
                if n != len(sizes):
                    err = f"ブロック数 {n} が BlockSize の {len(sizes)} と合わない"
                for k, v in tails.items():
                    if v != len(sizes):
                        err = f"{k} の要素数 {v} がブロック数 {len(sizes)} と合わない"
        except Exception as e:
            err = f"サブレコード列が壊れている ({e})"
        if err:
            lay_issue(lid, sev, f"{label}: {err}")
        return st, err is not None

    # --- ミップ連鎖 1 本の検査 --------------------------------------------
    def check_chain(lid, mm, which):
        """返り値は (段数, 100% 段の stats, 本体破損か, 段数不一致か)。"""
        sev = "remove" if which == "描画" else "repair"
        if mm not in mipmaps:
            lay_issue(lid, sev, f"{which}ミップ #{mm} が Mipmap に無い")
            return 0, None, True, False
        base, count = cur.execute(
            "SELECT BaseMipmapInfo, MipmapCount FROM Mipmap WHERE MainId=?",
            (mm,)).fetchone()
        hops = 0
        first = None
        broken = False
        node = base
        chain_seen = set()
        while node:
            if node in chain_seen or hops > CHAIN_LIMIT:
                lay_issue(lid, sev, f"{which}ミップ連鎖に閉路 (MipmapInfo #{node})")
                broken = True
                break
            if node not in infos:
                lay_issue(lid, sev, f"{which}ミップ連鎖が MipmapInfo #{node} で切れる")
                broken = True
                break
            chain_seen.add(node)
            o, nxt = cur.execute(
                "SELECT Offscreen, NextIndex FROM MipmapInfo WHERE MainId=?",
                (node,)).fetchone()
            st, bad = check_offscreen(lid, o, sev,
                                      f"{which} {'100%' if hops == 0 else '縮小'}段")
            broken |= bad
            if hops == 0:
                first = st
            node = nxt
            hops += 1
        mismatch = not broken and count != hops
        if mismatch:
            lay_issue(lid, "repair", f"{which} Mipmap #{mm}.MipmapCount={count} が"
                                     f"実際の段数 {hops} と違う (CSP が落ちる)")
            d["repairs"].append(("mipcount", mm, hops))
        return hops, first, broken, mismatch

    # --- サムネイル 1 枚の検査 --------------------------------------------
    def check_thumb(lid, tn, col, which):
        bad = False
        if tn not in thumbs:
            lay_issue(lid, "repair", f"{which}サムネイル #{tn} が LayerThumbnail に無い")
            bad = True
        else:
            o = cur.execute("SELECT ThumbnailOffscreen FROM LayerThumbnail"
                            " WHERE MainId=?", (tn,)).fetchone()[0]
            if o:
                _st, bad = check_offscreen(lid, o, "repair", f"{which}サムネイル")
        if bad:
            d["repairs"].append(("thumb", lid, col))

    # --- レイヤごと ---------------------------------------------------------
    for lid, depth in order:
        r = layers[lid]
        opa = r["LayerOpacity"]
        if opa is None or not (0 <= opa <= 256):
            lay_issue(lid, "repair", f"LayerOpacity={opa} が範囲 0..256 の外")
            d["repairs"].append(("opacity", lid))

        hops = 0
        st100 = None
        if r["LayerRenderMipmap"]:
            hops, st100, _broken, _mm = check_chain(lid, r["LayerRenderMipmap"], "描画")
        if r["LayerLayerMaskMipmap"]:
            _h, _s, broken, _mm = check_chain(lid, r["LayerLayerMaskMipmap"], "マスク")
            if broken:
                d["repairs"].append(("mask", lid))
        if r["LayerRenderThumbnail"]:
            check_thumb(lid, r["LayerRenderThumbnail"], "LayerRenderThumbnail", "描画")
        if r["LayerLayerMaskThumbnail"]:
            check_thumb(lid, r["LayerLayerMaskThumbnail"], "LayerLayerMaskThumbnail",
                        "マスク")

        d["layers"].append({
            "id": lid, "depth": depth, "name": as_str(r["LayerName"] or ""),
            "folder": bool(r["LayerFolder"]), "vis": r["LayerVisibility"],
            "opacity": opa, "composite": r["LayerComposite"],
            "mips": hops, "stats": st100,
        })

    if d["unreachable"]:
        glob("repair", f"ツリーから到達できないレイヤ: "
                       f"{d['unreachable']} (--fix で除去)")

    # --- 孤児行 (LayerId が Layer に無い) ----------------------------------
    orphan_total = 0
    for table in ("Offscreen", "Mipmap", "MipmapInfo", "LayerThumbnail"):
        n = cur.execute(
            f"SELECT COUNT(*) FROM [{table}] WHERE LayerId NOT IN"
            f" (SELECT MainId FROM Layer)").fetchone()[0]
        if n:
            glob("repair", f"{table} に孤児行 (LayerId が Layer に無い) {n} 行")
            orphan_total += n
    if orphan_total:
        d["repairs"].append(("orphans",))

    # --- 格納型 -------------------------------------------------------------
    t = sorted(r[0] for r in cur.execute(
        "SELECT DISTINCT typeof(BlockData) FROM Offscreen WHERE BlockData IS NOT NULL"))
    if t not in ([], ["blob"]):
        glob("repair", f"Offscreen.BlockData の格納型が {t} (CSP は blob のみ。"
                       f"text だとそのレイヤが全面透明になる)")
        d["repairs"].append(("typeof_blockdata",))
    t = sorted(r[0] for r in cur.execute(
        "SELECT DISTINCT typeof(ExternalID) FROM ExternalChunk"))
    if t not in ([], ["text"]):
        glob("repair", f"ExternalChunk.ExternalID の格納型が {t} (CSP は text のみ)")
        d["repairs"].append(("typeof_extid",))

    # --- ExternalChunk の行 / 実チャンク / オフセット ----------------------
    chunk_pos = {}
    try:
        for kind, off, _clen, info in walk_chunks(c.raw):
            if kind == "CHNKExta":
                chunk_pos[info["external_id"]] = off
    except Exception as e:
        d["fatal"] = f"チャンク列が壊れている: {e}"
        return d
    row_off = {as_str(e): o for e, o in
               cur.execute("SELECT ExternalID, Offset FROM ExternalChunk")}
    missing = set(row_off) - set(chunk_pos)
    extra = set(chunk_pos) - set(row_off)
    stale = [e for e in set(row_off) & set(chunk_pos) if row_off[e] != chunk_pos[e]]
    if missing:
        glob("repair", f"実チャンクの無い ExternalChunk 行が {len(missing)} 件"
                       f" (書き出しで消える)")
    if extra:
        glob("repair", f"ExternalChunk に載っていない実チャンクが {len(extra)} 件"
                       f" (書き出しで登録される)")
    if stale:
        glob("repair", f"ExternalChunk.Offset が実位置とずれている行が {len(stale)} 件"
                       f" (書き出しで再計算される)")
    if missing or extra or stale:
        d["repairs"].append(("sync",))

    unused = set(row_off) & set(chunk_pos) - _used_external_ids(cur)
    if unused:
        glob("info", f"どの列からも指されていない実体が {len(unused)} 件"
                     f" (CSP のファイルにもあるので異常ではない。--prune で破棄)")

    d["repairs"] = list(dict.fromkeys(map(tuple, d["repairs"])))
    return d


# --------------------------------------------------------------------------
# 除去と修復
# --------------------------------------------------------------------------

def _sweep_rows(cur, extmap, table, where, params):
    """行を消し、その行が持っていた external_id を集めて返す。"""
    cols = _table_cols(cur, table)
    ext = set()
    for col in extmap.get(table, ()):
        if col in cols:
            ext |= {as_str(v) for v, in cur.execute(
                f"SELECT [{col}] FROM [{table}] WHERE {where}"
                f" AND [{col}] IS NOT NULL", params)}
    n = cur.execute(f"SELECT COUNT(*) FROM [{table}] WHERE {where}",
                    params).fetchone()[0]
    if n:
        cur.execute(f"DELETE FROM [{table}] WHERE {where}", params)
    return n, ext


def _fix_current_layer(cur):
    """`Canvas.CanvasCurrentLayer` が死んでいたらルート直下の最上段へ向ける。"""
    row = cur.execute("SELECT MainId, CanvasRootFolder, CanvasCurrentLayer"
                      " FROM Canvas").fetchone()
    if row is None:
        return
    cid, root, current = row
    layers = _ids(cur, "Layer")
    if current in layers:
        return
    node = cur.execute("SELECT LayerFirstChildIndex FROM Layer WHERE MainId=?",
                       (root,)).fetchone()
    node = node[0] if node else 0
    top = node
    seen = set()
    while node and node in layers and node not in seen:
        seen.add(node)
        top = node
        node = cur.execute("SELECT LayerNextIndex FROM Layer WHERE MainId=?",
                           (node,)).fetchone()[0]
    cur.execute("UPDATE Canvas SET CanvasCurrentLayer=? WHERE MainId=?",
                (top or root, cid))


def remove_layers(c, ids):
    """レイヤ (フォルダなら子孫ごと) を消す。tolerant 版。

    `clip_write.delete_layer` と違い、連鎖が壊れていても最後まで進む。
    `LayerId` 列を持つ**全テーブル**から行を消すので、マスク連鎖や
    テキスト・ベクタの第 3 カテゴリ Offscreen (FK が無い) も残らない。
    返り値は (消したレイヤ ID 集合, 行が持っていた external_id 集合)。
    """
    cur = c.db.cursor()
    extmap = _extmap(cur)
    layers = {r[0]: (r[1] or 0, r[2] or 0) for r in cur.execute(
        "SELECT MainId, LayerFirstChildIndex, LayerNextIndex FROM Layer")}
    root = cur.execute("SELECT CanvasRootFolder FROM Canvas").fetchone()[0]

    doomed = set()

    def subtree(i):
        if i not in layers or i in doomed:
            return
        doomed.add(i)
        kid = layers[i][0]
        while kid and kid in layers and kid not in doomed:
            nxt = layers[kid][1]
            subtree(kid)
            kid = nxt

    for i in ids:
        subtree(i)
    if root in doomed:
        raise ValueError(f"ルートフォルダ #{root} は消せない")

    def next_alive(i):
        hop = set()
        while i and i in doomed and i not in hop:
            hop.add(i)
            i = layers.get(i, (0, 0))[1]
        return i if i in layers and i not in doomed else 0

    # 生き残る側のリンクから doomed を飛ばす
    for mid, (kid, nxt) in layers.items():
        if mid in doomed:
            continue
        if nxt in doomed:
            cur.execute("UPDATE Layer SET LayerNextIndex=? WHERE MainId=?",
                        (next_alive(nxt), mid))
        if kid in doomed:
            cur.execute("UPDATE Layer SET LayerFirstChildIndex=? WHERE MainId=?",
                        (next_alive(kid), mid))

    dropped = set()
    tables = [r[0] for r in cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")]
    for t in tables:
        if t == "Layer" or "LayerId" not in _table_cols(cur, t):
            continue
        for i in doomed:
            _n, e = _sweep_rows(cur, extmap, t, "LayerId=?", (i,))
            dropped |= e
    for i in doomed:
        cur.execute("DELETE FROM Layer WHERE MainId=?", (i,))
    _fix_current_layer(cur)
    c.db.commit()
    return doomed, dropped


def purge_orphans(c):
    """LayerId が Layer に無い孤児行を掃除する (clip_validate と同じ 4 テーブル)。"""
    cur = c.db.cursor()
    extmap = _extmap(cur)
    total = 0
    dropped = set()
    for t in ("Offscreen", "Mipmap", "MipmapInfo", "LayerThumbnail"):
        n, e = _sweep_rows(cur, extmap, t,
                           "LayerId NOT IN (SELECT MainId FROM Layer)", ())
        total += n
        dropped |= e
    c.db.commit()
    return total, dropped


def _drop_chain(cur, extmap, mm):
    """ミップ連鎖 1 本を丸ごと消す (tolerant)。"""
    ext = set()
    row = cur.execute("SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?",
                      (mm,)).fetchone()
    node = row[0] if row else 0
    seen = set()
    while node and node not in seen:
        seen.add(node)
        r = cur.execute("SELECT Offscreen, NextIndex FROM MipmapInfo WHERE MainId=?",
                        (node,)).fetchone()
        cur.execute("DELETE FROM MipmapInfo WHERE MainId=?", (node,))
        if r is None:
            break
        o, nxt = r
        if o:
            _n, e = _sweep_rows(cur, extmap, "Offscreen", "MainId=?", (o,))
            ext |= e
        node = nxt
    cur.execute("DELETE FROM Mipmap WHERE MainId=?", (mm,))
    return ext


def _drop_thumb(cur, extmap, lid, col):
    """サムネイル一式 (行 + Offscreen + 実体) を消してポインタを 0 に。

    行ごと消しても CSP は開いたときに作り直す [実測: DOCTOR_TEST ⑤]。
    """
    ext = set()
    row = cur.execute(f"SELECT [{col}] FROM Layer WHERE MainId=?", (lid,)).fetchone()
    tn = row[0] if row else 0
    if tn:
        r = cur.execute("SELECT ThumbnailOffscreen FROM LayerThumbnail WHERE MainId=?",
                        (tn,)).fetchone()
        if r and r[0]:
            _n, e = _sweep_rows(cur, extmap, "Offscreen", "MainId=?", (r[0],))
            ext |= e
        cur.execute("DELETE FROM LayerThumbnail WHERE MainId=?", (tn,))
    cur.execute(f"UPDATE Layer SET [{col}]=0 WHERE MainId=?", (lid,))
    return ext


def _heal_layer_pointers(cur):
    """行ごと消えた連鎖・サムネイルへのポインタを 0 に倒す (掃除後の安全網)。"""
    for col, table in (("LayerRenderThumbnail", "LayerThumbnail"),
                       ("LayerLayerMaskThumbnail", "LayerThumbnail"),
                       ("LayerLayerMaskMipmap", "Mipmap")):
        if col in _table_cols(cur, "Layer"):
            cur.execute(f"UPDATE Layer SET [{col}]=0 WHERE [{col}]>0 AND [{col}]"
                        f" NOT IN (SELECT MainId FROM [{table}])")


def apply_fix(c, d, extra_remove=(), auto=True, prune=False):
    """診断結果を適用する。返り値は (消したレイヤ集合, 実施内容の説明リスト)。"""
    cur = c.db.cursor()
    extmap = _extmap(cur)
    dropped = set()
    acts = []

    if auto:
        for rep in d["repairs"]:
            tag = rep[0]
            if tag == "mipcount":
                cur.execute("UPDATE Mipmap SET MipmapCount=? WHERE MainId=?",
                            (rep[2], rep[1]))
                acts.append(f"Mipmap #{rep[1]}.MipmapCount を {rep[2]} に直した")
            elif tag == "opacity":
                cur.execute("UPDATE Layer SET LayerOpacity="
                            "MAX(0, MIN(256, COALESCE(LayerOpacity, 256)))"
                            " WHERE MainId=?", (rep[1],))
                acts.append(f"レイヤ #{rep[1]} の LayerOpacity を 0..256 に丸めた")
            elif tag == "deadlink":
                cur.execute(f"UPDATE Layer SET [{rep[2]}]=0 WHERE MainId=?", (rep[1],))
                acts.append(f"#{rep[1]}.{rep[2]} の死んだリンクを切った")
            elif tag == "typeof_blockdata":
                rows = cur.execute("SELECT MainId, BlockData FROM Offscreen"
                                   " WHERE typeof(BlockData)='text'").fetchall()
                for mid, v in rows:
                    cur.execute("UPDATE Offscreen SET BlockData=? WHERE MainId=?",
                                (v.encode("ascii"), mid))
                acts.append(f"Offscreen.BlockData {len(rows)} 行を blob に直した")
            elif tag == "typeof_extid":
                rows = cur.execute("SELECT rowid, ExternalID FROM ExternalChunk"
                                   " WHERE typeof(ExternalID)='blob'").fetchall()
                for rid, v in rows:
                    cur.execute("UPDATE ExternalChunk SET ExternalID=? WHERE rowid=?",
                                (as_str(v), rid))
                acts.append(f"ExternalChunk.ExternalID {len(rows)} 行を text に直した")
            elif tag == "mask":
                lid = rep[1]
                mm = cur.execute("SELECT LayerLayerMaskMipmap FROM Layer"
                                 " WHERE MainId=?", (lid,)).fetchone()
                if mm and mm[0]:
                    dropped |= _drop_chain(cur, extmap, mm[0])
                cur.execute("UPDATE Layer SET LayerLayerMaskMipmap=0 WHERE MainId=?",
                            (lid,))
                dropped |= _drop_thumb(cur, extmap, lid, "LayerLayerMaskThumbnail")
                acts.append(f"レイヤ #{lid} の壊れたマスクを外した")
            elif tag == "thumb":
                dropped |= _drop_thumb(cur, extmap, rep[1], rep[2])
                acts.append(f"レイヤ #{rep[1]} の壊れたサムネイルを落とした"
                            f" (CSP が作り直す)")
            # "curlayer" / "orphans" は下で、"sync" は save() が処理する

    removals = set(extra_remove)
    if auto:
        removals |= d["removals"] | set(d["unreachable"])
    removed = set()
    if removals:
        removed, e = remove_layers(c, removals)
        dropped |= e
        acts.append("レイヤを除去した: " + " ".join(f"#{i}" for i in sorted(removed)))

    if auto:
        n, e = purge_orphans(c)
        dropped |= e
        if n:
            acts.append(f"孤児行 {n} 行を掃除した")
        _heal_layer_pointers(cur)

    _fix_current_layer(cur)

    if prune:
        used = _used_external_ids(cur)
        before = len(c.externals)
        c.externals = [(e, p) for e, p in c.externals if as_str(e) in used]
        if before - len(c.externals):
            acts.append(f"未参照の実体 {before - len(c.externals)} 件を破棄した")

    # 消した行が持っていた実体のうち、もうどこからも参照されないものだけ落とす
    if dropped:
        used = _used_external_ids(cur)
        kill = dropped - used
        c.externals = [(e, p) for e, p in c.externals if as_str(e) not in kill]

    c.db.commit()
    return removed, acts


# --------------------------------------------------------------------------
# 表示と CLI
# --------------------------------------------------------------------------

SEV_MARK = {"remove": "NG[除去候補]", "repair": "NG[修復可能]", "info": "--[情報]"}


def report(d, path, nbytes):
    print(f"{path}  ({nbytes:,} B)")
    s = d["stats"]
    if d["fatal"]:
        print(f"  FATAL {d['fatal']}")
        return
    print(f"  レイヤ {s['layers']} / Offscreen {s['offscreens']}"
          f" / 実体チャンク {s['chunks']}")
    print()
    for e in d["layers"]:
        kind = "folder" if e["folder"] else "layer"
        st = e["stats"]
        blocks = f" blocks {st['data']}/{st['total']}" if st else ""
        mips = f" mip{e['mips']}段" if e["mips"] else ""
        print(f"  {'  ' * e['depth']}#{e['id']} {e['name']!r} [{kind}]"
              f" vis={e['vis']} opa={e['opacity']} comp={e['composite']}"
              f"{mips}{blocks}")
        for sev, msg in d["layer_issues"].get(e["id"], []):
            print(f"  {'  ' * e['depth']}   {SEV_MARK[sev]} {msg}")
    for lid in d["unreachable"]:
        print(f"  (到達不能) #{lid}")
        for sev, msg in d["layer_issues"].get(lid, []):
            print(f"     {SEV_MARK[sev]} {msg}")
    print()
    n_rem = len(d["removals"]) + len(d["unreachable"])
    n_rep = len(d["repairs"])
    for sev, msg in d["global"]:
        print(f"  {SEV_MARK[sev]} {msg}")
    if n_rem:
        ids = sorted(d["removals"] | set(d["unreachable"]))
        print(f"  除去候補のレイヤ: {' '.join(f'#{i}' for i in ids)}"
              f"  (--fix か --remove ID --out OUT.clip で除去)")
    if n_rep:
        print(f"  修復可能な問題: {n_rep} 件  (--fix --out OUT.clip で修復)")
    if not n_rem and not n_rep:
        print("  => 不正は見つからなかった")


def has_problems(d):
    return bool(d["fatal"] or d["removals"] or d["unreachable"] or d["repairs"])


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("--deep", action="store_true",
                    help="全ブロックを zlib 展開して照合する (大きいファイルでは遅い)")
    ap.add_argument("--fix", action="store_true",
                    help="修復可能を直し、除去候補と到達不能レイヤを取り除く (--out 必須)")
    ap.add_argument("--remove", action="append", type=int, default=[], metavar="ID",
                    help="指定レイヤを除去する。複数可 (--out 必須)")
    ap.add_argument("--prune", action="store_true",
                    help="どの列からも指されていない実体チャンクも破棄する")
    ap.add_argument("--out", metavar="OUT.clip", help="書き出し先")
    ap.add_argument("--no-preview", action="store_true",
                    help="レイヤ除去後の CanvasPreview 再合成をしない")
    args = ap.parse_args(argv)

    write = args.fix or args.remove or args.prune
    if write and not args.out:
        ap.error("--fix / --remove / --prune には --out OUT.clip が要る")
    if args.out and not write:
        ap.error("--out には --fix / --remove / --prune のいずれかが要る")

    c = ClipFile(args.src)
    d = diagnose(c, deep=args.deep)
    report(d, args.src, len(c.raw))
    if d["fatal"]:
        c.close()
        return 2

    if not write:
        c.close()
        return 1 if has_problems(d) else 0

    for i in args.remove:
        row = c.db.execute("SELECT MainId FROM Layer WHERE MainId=?", (i,)).fetchone()
        if row is None:
            print(f"  エラー: レイヤ #{i} が無い")
            c.close()
            return 2

    removed, acts = apply_fix(c, d, extra_remove=args.remove,
                              auto=args.fix, prune=args.prune)
    n = c.save(args.out)
    c.close()
    print()
    for a in acts:
        print(f"  {a}")
    print(f"  {args.out}  {n:,} B")

    if removed and not args.no_preview:
        # レイヤが減ると合成結果が変わる。CSP は開いた直後 CanvasPreview を
        # 表示するので、作り直せる環境 (numpy + imgdoc) なら作り直す。
        try:
            try:
                from .clip_write import refresh_preview
            except ImportError:
                from clip_write import refresh_preview
            refresh_preview(args.out)
            print("  CanvasPreview を再合成した")
        except Exception as e:
            print(f"  警告: CanvasPreview を再合成できなかった ({e})。"
                  f"開いた直後だけ古い絵が出る")

    try:
        from .clip_validate import validate
    except ImportError:
        from clip_validate import validate
    bad = validate(args.out, verbose=False)
    if bad:
        print("  clip_validate: NG")
        for b in bad:
            print(f"    NG {b}")
        return 1
    print("  clip_validate: OK (参照整合性が通った)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
