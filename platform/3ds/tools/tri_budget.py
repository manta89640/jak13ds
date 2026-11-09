#!/usr/bin/env python3
"""Tables from the host soft renderer's [tb] lines (CTR_SOFT_STATS=N gk --ctr-gfx ...).

  tri_budget.py LOG [--view NAME] [--md] [--tex] [--all-windows]

A view is the span between the pad script's "log view-begin NAME" and "log view-end NAME"; its
numbers are the last statistics window that ends inside it, per frame.
"""
import re
import sys
from collections import defaultdict

FATES = ["near", "off", "back", "nopix", "kill", "hid", "vis"]
BANDS = ["0-10", "10-20", "20-40", "40-70", "70-120", "120-250", "250-500", "500+", "none"]
FMT = ["RGB565", "RGBA4", "ETC1", "ETC1A4", "RGBA8", "A8", "A4", "RGBA5551", "runtime"]
TILE_BYTES = [128, 128, 32, 64, 256, 64, 32, 128, 128]
AREA = ["<1/8", "<1/2", "<1", "<2", "<4", "<16", "<64", ">=64"]
GROUPS = ["tfrag", "tie", "shrub", "vista", "merc", "ocean", "sky", "sprites", "shadow", "hud",
          "other"]


def group(name):
    base = name.split("@")[0].split("+")[0]
    if re.match(r"l\d/", base):
        kind = base.split("/")[1]
        if kind.startswith("tfrag"):
            return "tfrag"
        if kind.startswith("tie") or kind in ("wind", "etie"):
            return "tie"
        if kind.startswith("shrub"):
            return "shrub"
        return "vista"
    if base.startswith("m:") or base.startswith("merc") or base.startswith("generic"):
        return "merc"
    low = base.lower()
    if "ocean" in low:
        return "ocean"
    if low == "sky" or "sky" in low:
        return "sky"
    if low == "sprite-hud" or "hud" in low or "progress" in low or "screen" in low:
        return "hud"
    if "sprite" in low or "particle" in low or "glow" in low:
        return "sprites"
    if "shadow" in low:
        return "shadow"
    return "other"


class Win:
    def __init__(self, frame, frames):
        self.frame, self.frames = frame, frames
        self.b = {}
        self.f = defaultdict(lambda: [[0.0] * 9 for _ in BANDS])
        self.a = {}
        self.x = defaultdict(lambda: defaultdict(lambda: [0.0] * 5))
        self.t = []


def parse(path):
    """-> [(view or None, Win)] in log order"""
    out = []
    view = None
    cur = None
    strip = re.compile(r"\x1b\[[0-9;]*m")
    for raw in open(path, "rb"):
        line = strip.sub("", raw.decode("latin1")).rstrip("\n")
        m = re.search(r"\[pad script\] frame \d+: view-(begin|end) (\S+)", line)
        if m:
            view = m.group(2) if m.group(1) == "begin" else None
            if m.group(1) == "begin":
                out.append((view, None))
            continue
        i = line.find("[tb] ")
        if i < 0:
            continue
        p = line[i + 5:].split()
        if p[0] == "win":
            cur = Win(int(p[2]), int(p[4]))
            cur.wpm = float(p[6])
        elif cur is None:
            continue
        elif p[0] == "b":
            cur.b[p[1]] = [float(v) for v in p[2:]]
        elif p[0] == "f":
            cur.f[p[1]][int(p[2])] = [float(v) for v in p[3:]]
        elif p[0] == "a":
            cur.a[p[1]] = [float(v) for v in p[2:]]
        elif p[0] == "x":
            cur.x[p[1]][(int(p[2]), int(p[3]))] = [float(v) for v in p[4:]]
        elif p[0] == "t":
            cur.t.append((int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6]),
                          float(p[7]), p[8] if len(p) > 8 else "?"))
        elif p[0] == "end":
            out.append((view, cur))
            cur = None
    return out


def views(path, all_windows=False):
    """-> {view: Win} (the last window inside each view span that began inside it)"""
    res = {}
    seen_in_span = {}
    for view, win in parse(path):
        if win is None:
            seen_in_span[view] = 0
            continue
        if view is None:
            if all_windows:
                res["frame%d" % win.frame] = win
            continue
        seen_in_span[view] = seen_in_span.get(view, 0) + 1
        if seen_in_span[view] >= 2:
            res[view] = win
    return res


def summarize(win):
    """per group: dict of per-frame numbers"""
    n = win.frames
    g = {k: defaultdict(float) for k in GROUPS}
    bands = {k: [[0.0] * 9 for _ in BANDS] for k in GROUPS}
    area = {k: [0.0] * 8 for k in GROUPS}
    # logs from before the envmapped tie draws had a bucket of their own: they are the triangles
    # without a view depth in the faded shrub bucket
    etie_in_fade = {}
    for name, rows in win.f.items():
        if name.endswith("/shrubfade"):
            etie_in_fade[name] = sum(rows[8][:7]) > sum(sum(r[:7]) for r in rows[:8])
    for name, b in win.b.items():
        k = "tie" if etie_in_fade.get(name) else group(name)
        d = g[k]
        for key, v in zip(["draws", "binds", "idx", "verts", "px", "pass", "final", "upos", "upst",
                           "upstc", "vinstr", "vbufs", "vc16", "vc16_keep", "blend", "atest", "flat",
                           "dup_idx", "dup_pos", "upstm", "upstmc", "zties"], b):
            d[key] += v / n
        if "@rt" in name:
            d["rt_px"] += b[4] / n
    for name, rows in win.f.items():
        for bi, row in enumerate(rows):
            k = "tie" if name.endswith("/shrubfade") and bi == 8 else group(name)
            for j, v in enumerate(row):
                bands[k][bi][j] += v / n
            for j, fate in enumerate(FATES):
                g[k][fate] += row[j] / n
    for name, a in win.a.items():
        k = "tie" if etie_in_fade.get(name) else group(name)
        for j, v in enumerate(a):
            area[k][j] += v / n
    for k in GROUPS:
        g[k]["tris"] = sum(g[k][f] for f in FATES)
    return g, bands, area


def fmt_k(v):
    if v >= 9950:
        return "%.0fk" % (v / 1000)
    if v >= 1000:
        return "%.1fk" % (v / 1000)
    return "%.0f" % v


def table_view(name, win):
    g, bands, area = summarize(win)
    out = ["### %s" % name, ""]
    out.append("| bucket | tris | verts | draws | binds | fragments (screens) | off screen | back face "
               "| no pixel | no fragment written | covered later | in the picture | final px |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    tot = defaultdict(float)
    for k in GROUPS:
        d = g[k]
        if not d["tris"] and not d["draws"]:
            continue
        for key in list(d.keys()):
            tot[key] += d[key]
        out.append(row_of(k, d))
    out.append(row_of("**all**", tot))
    out.append("")
    lv = ["tfrag", "tie", "shrub", "vista", "merc"]
    out.append("Triangles by view depth (m), level and merc buckets: all / no pixel / in the picture")
    out.append("")
    out.append("| bucket | " + " | ".join(BANDS[:8]) + " |")
    out.append("|---|" + "---|" * 8)
    for k in lv:
        if not g[k]["tris"]:
            continue
        cells = []
        for bi in range(8):
            r = bands[k][bi]
            t = sum(r[:7])
            cells.append("%s / %s / %s" % (fmt_k(t), fmt_k(r[3]), fmt_k(r[6])) if t >= 0.5 else "-")
        out.append("| %s | %s |" % (k, " | ".join(cells)))
    out.append("")
    return "\n".join(out)


def row_of(k, d):
    off = d["off"] + d["near"]
    return "| %s | %s | %s | %.0f | %.0f | %s (%.2f) | %s | %s | %s | %s | %s | %s | %s |" % (
        k, fmt_k(d["tris"]), fmt_k(d["verts"]), d["draws"], d["binds"], fmt_k(d["px"]),
        d["px"] / 96000.0, fmt_k(off), fmt_k(d["back"]), fmt_k(d["nopix"]), fmt_k(d["kill"]),
        fmt_k(d["hid"]), fmt_k(d["vis"]), fmt_k(d["final"]))


def binds_table(name, win):
    """binds and different textures per bucket group, vertices fetched per attribute stream"""
    g, bands, area = summarize(win)
    n = win.frames
    ntex = defaultdict(int)
    for t in win.t:
        ntex[group(t[7])] += 1
    out = ["| bucket | draws | texture binds | different textures (first use) | vertices: stream 0 | "
           "stream 1 (colors) | stream 2 (morph / wind records) | program instructions per vertex |",
           "|---|---|---|---|---|---|---|---|"]
    for k in GROUPS:
        d = g[k]
        if not d["draws"]:
            continue
        lvl = k in ("tfrag", "tie", "shrub", "vista")
        v = d["verts"]
        s2 = 0.0
        for bname, b in win.b.items():
            if group(bname) == k and (bname.endswith("+morph") or bname.endswith("+tmorph") or
                                      bname.endswith("/wind") or
                                      bname.endswith("/etie")):
                s2 += b[3] / n
        out.append("| %s | %.0f | %.0f | %d | %s | %s | %s | %.1f |" % (
            k, d["draws"], d["binds"], ntex[k], fmt_k(v), fmt_k(v) if lvl else "-",
            fmt_k(s2) if lvl else "-", d["vinstr"] / v if v else 0))
    out.append("")
    return "\n".join(out)


def extra_table(name, win):
    g, bands, area = summarize(win)
    n = win.frames
    out = ["### %s: vertices and passes" % name, ""]
    out.append("| bucket | tris | verts / tri | by position | + texture coords | + color now | "
               "program instr / vertex | buffers / vertex | cache 16 cleared / tri | kept / tri | "
               "no area | same 3 vertices | same 3 positions | blended px |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for k in GROUPS:
        d = g[k]
        if not d["tris"] or not d["verts"]:
            continue
        v, t = d["verts"], d["idx"] / 3.0
        lvl = k in ("tfrag", "tie", "shrub", "vista")
        out.append("| %s | %s | %.2f | %s | %s | %s | %.1f | %.2f | %s | %s | %s | %s | %s | %s |" % (
            k, fmt_k(t), v / t,
            "%.0f%%" % (100 * d["upos"] / v) if lvl else "", "%.0f%%" % (100 * d["upst"] / v) if lvl else "",
            "%.0f%%" % (100 * d["upstc"] / v) if lvl else "", d["vinstr"] / v, d["vbufs"] / v,
            "%.2f" % (d["vc16"] / t) if lvl else "", "%.2f" % (d["vc16_keep"] / t) if lvl else "",
            fmt_k(d["flat"]), fmt_k(d["dup_idx"]), fmt_k(d["dup_pos"]), fmt_k(d["blend"])))
    out.append("")
    out.append("| pass (raw bucket) | draws | binds | tris | verts | fragments |")
    out.append("|---|---|---|---|---|---|")
    for bname in sorted(win.b):
        b = win.b[bname]
        if bname.startswith("l") and "/" in bname:
            out.append("| %s | %.0f | %.0f | %s | %s | %s |" % (
                bname, b[0] / n, b[1] / n, fmt_k(b[2] / 3 / n), fmt_k(b[3] / n), fmt_k(b[4] / n)))
    out.append("")
    out.append("| area of rasterized triangles (px) | " + " | ".join(AREA) + " |")
    out.append("|---|" + "---|" * 8)
    for k in GROUPS:
        if sum(area[k]) >= 1:
            out.append("| %s | %s |" % (k, " | ".join(fmt_k(v) for v in area[k])))
    out.append("")
    return "\n".join(out)


def tex_table(name, win):
    n = win.frames
    per = defaultdict(lambda: defaultdict(lambda: [0.0] * 5))
    for bname, rows in win.x.items():
        k = group(bname)
        k = "level" if k in ("tfrag", "tie", "shrub", "vista") else k
        for (f, l), v in rows.items():
            for j in range(5):
                per[k][(f, l)][j] += v[j] / n
    out = ["### %s: texture reads" % name, ""]
    out.append("| bucket | format | mip | fragments | tiles / frame | tiles / bind | KB / bind-run "
               "| bias 0.5 KB | bias 1 KB |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    tot = defaultdict(lambda: [0.0] * 4)
    for k in sorted(per):
        for (f, l), v in sorted(per[k].items()):
            kb = [v[j] * TILE_BYTES[f] / 1024.0 for j in (2, 3, 4)]
            tot[k][0] += v[0]
            for j in range(3):
                tot[k][1 + j] += kb[j]
            if v[0] < 200 and kb[0] < 1:
                continue
            out.append("| %s | %s | %d | %s | %.0f | %.0f | %.1f | %.1f | %.1f |" % (
                k, FMT[f], l, fmt_k(v[0]), v[1], v[2], kb[0], kb[1], kb[2]))
    for k in sorted(tot):
        out.append("| **%s** | all | | %s | | | %.0f | %.0f | %.0f |" % (
            k, fmt_k(tot[k][0]), tot[k][1], tot[k][2], tot[k][3]))
    out.append("")
    return "\n".join(out)


def main():
    args = sys.argv[1:]
    md = "--md" in args
    tex = "--tex" in args
    allw = "--all-windows" in args
    only = args[args.index("--view") + 1] if "--view" in args else None
    path = [a for a in args if not a.startswith("--") and a != only][0]
    for name, win in views(path, allw).items():
        if only and name != only:
            continue
        print(table_view(name, win))
        if "--binds" in args:
            print(binds_table(name, win))
        if "--extra" in args:
            print(extra_table(name, win))
        if tex:
            print(tex_table(name, win))


if __name__ == "__main__":
    main()
