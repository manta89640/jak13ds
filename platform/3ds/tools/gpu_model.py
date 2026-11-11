#!/usr/bin/env python3
"""GPU cost model of the 3DS renderer (PICA200, New 3DS): docs/3ds-port/gpu_model.md.

  gpu_model.py hwfit LOG... [--out calib.json]   fit hardware stats windows, cross-validated
  gpu_model.py bench LOG [--out costs.json]       per-feature costs from pica_bench [bench] lines
  gpu_model.py predict HOSTLOG... [--calib calib.json] [--costs costs.json]
                                                  predicted hardware GPU ms per host window/view
  gpu_model.py draws DRAWS.csv [--costs ...]      the costliest draws of a CTR_GPU_MODEL_DRAWS file

No third-party modules.
"""
import argparse
import glob
import json
import math
import os
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CALIB = os.path.join(HERE, "gpu_model_calib.json")

# ---------------------------------------------------------------------------------------------
# features and the physical model
#
# Host feature names ([gpumodel] b lines, per frame) and their unit cost in ns at the PICA200's
# 268 MHz (3.73 ns per cycle). PRIOR: first-principles guesses, for the share of each feature
# until pica_bench measures it (bench -> costs.json overrides each one it measured).
CYCLE_NS = 1e3 / 268.111
PRIOR = {
    # per frame: color + depth fill of the 400x240 target (2 memory fill units, VRAM ~ 4 B/cycle),
    # the display transfer (RGBA8 -> RGB8, ~1 px/cycle) and the fence copy
    "frame": 2.0e6,
    "draws": 200 * CYCLE_NS,       # ~30 register writes per draw, command processor ~4 cycles each, plus the draw kick
    "binds": 300 * CYCLE_NS,       # texture registers + texture cache clear (pipeline drains to the texture unit)
    "stchg": 60 * CYCLE_NS,        # blend / depth / combiner registers of a state change
    "progsw": 400 * CYCLE_NS,      # program switch: code / operand descriptors and uniforms re-sent
    "unif": 2 * CYCLE_NS,          # a uniform vector (4 words through the command processor)
    "tri_in": 4 * CYCLE_NS,        # primitive assembly, clip and cull test of every submitted triangle
    "tri_setup": 12 * CYCLE_NS,    # triangle setup of one that reaches the rasterizer
    "vmiss": 8 * CYCLE_NS,         # a vertex shaded: attribute fetch + output (post-vertex cache miss)
    "vinstr": 0.5 * CYCLE_NS,      # a vertex program instruction (4 units, ~2 cycles issue each)
    "vbufs": 2 * CYCLE_NS,         # an attribute buffer fetched for a vertex (separate memory stream)
    "px": 1.0 * CYCLE_NS,          # a fragment rasterized, depth read / tested (late: after texturing)
    "px_tex": 1.0 * CYCLE_NS,      # a fragment textured (unit 0)
    "px_u2": 1.0 * CYCLE_NS,       # a fragment reading a second texture unit
    "px_z": 0.5 * CYCLE_NS,        # a fragment passing: color (+ depth) write
    "px_blend": 0.5 * CYCLE_NS,    # blending: the color buffer read
    "tex_kb16": 1024 / 1.0 * CYCLE_NS,  # texture bytes missed by a 16-tile cache, ~1 B/cycle from FCRAM / VRAM
    "copy_rows": 2000 * CYCLE_NS,  # the distort screen copy, per 8-pixel band of the 400 (240 x 8 RGBA8)
}
FEATURES = list(PRIOR)
# the hardware stats windows carry these (render ms/frame + gpumodel hw lines); host equivalents
HW_FEATURES = {"tris": "tri_in", "draws": "draws", "binds": "binds"}

# feature groups for the report
GROUPS = {
    "frame": ["frame", "copy_rows"],
    "commands": ["draws", "binds", "stchg", "progsw", "unif"],
    "vertex": ["vmiss", "vinstr", "vbufs"],
    "triangles": ["tri_in", "tri_setup"],
    "fill": ["px", "px_tex", "px_u2", "px_z", "px_blend", "tex_kb16"],
}


# ---------------------------------------------------------------------------------------------
# small linear algebra (no numpy)
def solve(M, v):
    n = len(v)
    A = [row[:] + [v[i]] for i, row in enumerate(M)]
    for i in range(n):
        p = max(range(i, n), key=lambda k: abs(A[k][i]))
        A[i], A[p] = A[p], A[i]
        if abs(A[i][i]) < 1e-12:
            A[i][i] = 1e-12
        for k in range(i + 1, n):
            f = A[k][i] / A[i][i]
            for j in range(i, n + 1):
                A[k][j] -= f * A[i][j]
    x = [0.0] * n
    for i in reversed(range(n)):
        x[i] = (A[i][n] - sum(A[i][j] * x[j] for j in range(i + 1, n))) / A[i][i]
    return x


def lsq(X, y, w=None, ridge=0.0):
    """weighted least squares, non-negative slopes by active-set (the intercept stays free)"""
    n = len(X[0])
    w = w or [1.0] * len(y)
    active = list(range(n))
    while True:
        M = [[sum(wi * x[i] * x[j] for x, wi in zip(X, w)) + (ridge if i == j and i else 0.0)
              for j in active] for i in active]
        v = [sum(wi * x[i] * yy for x, yy, wi in zip(X, y, w)) for i in active]
        co = solve(M, v)
        neg = [a for a, c in zip(active, co) if a > 0 and c < 0]
        if not neg:
            full = [0.0] * n
            for a, c in zip(active, co):
                full[a] = c
            return full
        active.remove(min(neg, key=lambda a: co[active.index(a)]))


def rms(e):
    return math.sqrt(sum(x * x for x in e) / len(e)) if e else float("nan")


# ---------------------------------------------------------------------------------------------
# hardware logs
RENDER = re.compile(
    r"render ms/frame over (\d+) frames: wait-gpu ([\d.]+), build ([\d.]+), submit ([\d.]+) "
    r"\(cpu total ([\d.]+); gpu-wait ([\d.]+)\);.*?busy ([\d.]+) \((\d+) frames; active ([\d.]+)\)\); "
    r"(\d+) triangles, (\d+) draws, (\d+) texture binds, (\d+) KB commands")
GMHW = re.compile(r"gpumodel hw: (\d+) vertex program kinstr, ([\d.]+) program switches, ([\d.]+) "
                  r"state changes \(([\d.]+) blended\)")
PERFLOG = re.compile(r"gpu perflog: .*?ms/frame: p3d ([\d.]+) ppf ([\d.]+) psc ([\d.]+)")
LEVELS = re.compile(r"memory \(frame \d+\):.*?GAME [\d/]+ KB(?: \(linear / heap KB\))?,? ?(.*)")


def folder_key(path):
    """chronological order of the report folders (hw-oct25, hw-oct31b, hw-nov01g, ...)"""
    m = re.search(r"(?:hw|logs)-(oct|nov)(\d+)([a-z]?)", path)
    if not m:
        return (9, 0, "", path)
    return (0 if m.group(1) == "oct" else 1, int(m.group(2)), m.group(3), path)


def read_hw(paths, min_frames=200, min_tris=3000):
    """stats windows of hardware gk.logs, each once (the logs are appended across sessions and
    copied into several report folders: a window keeps the first folder it appears in)"""
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, "**", "*gk*.log"), recursive=True)) if os.path.isdir(p) else [p]
    files.sort(key=folder_key)
    seen, rows = set(), []
    for f in files:
        game = "jak2" if "jak2" in f else "jak1"
        sess, levels, pending, nsess = None, "", None, 0
        for line in open(f, errors="replace"):
            line = ANSI.sub("", line)
            if "settings from" in line and "[ctr]" in line:
                nsess += 1
                sess = (nsess, line[line.index("settings from"):][:300])
                continue
            m = LEVELS.search(line)
            if m:
                levels = " ".join(re.findall(r"([a-z][a-z0-9-]+) \d+/\d+ KB", m.group(1)))
                continue
            m = re.search(r"textures: \d+ level pools (\d+) KB, (\d+) in VRAM", line)
            if m and pending is not None:
                pending["pools_kb"], pending["pools_vram"] = int(m.group(1)), int(m.group(2))
                continue
            m = PERFLOG.search(line)
            if m and pending is not None:
                # config.ini gpu_perflog: the GPU engines' own time, an exact target
                gpu = float(m.group(1)) + float(m.group(2)) + float(m.group(3))
                if gpu > 0.5:
                    pending.update(active=gpu, gpu_bound=True, perflog=True, cpu_near=False)
                continue
            m = GMHW.search(line)
            if m and pending is not None:
                pending.update(vinstr=float(m.group(1)) * 1000, progsw=float(m.group(2)),
                               stchg=float(m.group(3)), blend_states=float(m.group(4)))
                continue
            m = RENDER.search(line)
            if not m:
                continue
            pending = None
            key = line[line.index("render ms"):]
            if key in seen:
                continue
            seen.add(key)
            g = [float(x) for x in m.groups()]
            if g[0] < min_frames or g[9] < min_tris or g[8] * g[0] > 1.3 * 15000:
                continue  # (a load stall: longer than the window)
            r = dict(file=f, game=game, sess=(f, sess), levels=levels, frames=g[0], wait_gpu=g[1],
                     build=g[2], cpu=g[4], busy=g[6], active=g[8], tris=g[9], draws=g[10],
                     binds=g[11], cmd_kb=g[12])
            r["folder"] = re.search(r"(hw-[a-z0-9-]+|logs-[a-z0-9-]+)", f).group(1) if re.search(r"(hw-[a-z0-9-]+|logs-[a-z0-9-]+)", f) else f
            # GPU-bound: under 29.3 fps, the render thread waited for the GPU and active is the frame
            # period (15 s windows); else active is only an upper bound (partial submits' bubbles)
            r["gpu_bound"] = g[0] < 440 and g[5] >= 0.3 and g[8] >= 0.9 * 15000.0 / g[0]
            # the render thread's own work near the period too: active may hold its partial
            # submits' bubbles (overlap = on)
            r["cpu_near"] = (g[4] - g[5]) >= 0.85 * 15000.0 / g[0]
            rows.append(r)
            pending = r
    return rows


def hw_x(r, cols):
    return [1.0] + [r[c] / (1000.0 if c == "tris" else 1.0) for c in cols]


def hwfit(rows, cols, w_free=1.0):
    """least squares on the GPU-bound windows (active = GPU time) with the others as upper bounds
    (GPU time <= active): a free window enters the fit only while the fit puts it above its active
    (hinge loss, iterated to a fixed point)"""
    bound = [r for r in rows if r["gpu_bound"]]
    free = [r for r in rows if not r["gpu_bound"]]
    used = set()
    co = None
    for _ in range(50):
        rs = bound + [free[i] for i in sorted(used)]
        if len(rs) < len(cols) + 2:
            rs = rows
        X = [hw_x(r, cols) for r in rs]
        y = [r["active"] for r in rs]
        w = [1.0 if r["gpu_bound"] else w_free for r in rs]
        co = lsq(X, y, w)
        viol = {i for i, r in enumerate(free) if hw_predict(co, r, cols) > r["active"]}
        new = used | viol
        if new == used:
            break
        used = new
    return co


def hw_predict(co, r, cols):
    return sum(c * x for c, x in zip(co, hw_x(r, cols)))


def cross_validate(rows, cols, group=lambda r: r["sess"]):
    """leave one group (session) out: the error on windows of a session the fit never saw"""
    errs = []
    for g in sorted(set(map(group, rows)), key=str):
        tr = [r for r in rows if group(r) != g]
        te = [r for r in rows if group(r) == g]
        if len(tr) < len(cols) + 5:
            continue
        co = hwfit(tr, cols)
        errs += [hw_predict(co, r, cols) - r["active"] for r in te if r["gpu_bound"]]
    return errs


def cmd_hwfit(a):
    rows = read_hw(a.logs)
    if a.since:
        rows = [r for r in rows if folder_key(r["folder"]) >= folder_key(a.since)]
    if not rows:
        sys.exit("no stats windows")
    bound = [r for r in rows if r["gpu_bound"]]
    free = [r for r in rows if not r["gpu_bound"]]
    print(f"{len(rows)} hardware windows from {len(set(r['sess'] for r in rows))} sessions, folders "
          f"{sorted(set(r['folder'] for r in rows), key=folder_key)}")
    print(f"{len(bound)} GPU-bound windows (under 29.3 fps, the render thread waited for the GPU, "
          f"active = the frame period) fitted; the other {len(free)} are upper bounds (their active "
          f"includes CPU build time after the frame's first partial submit): hinge fit\n")
    sets = [["tris"], ["draws"], ["binds"], ["tris", "draws"], ["tris", "binds"], ["tris", "draws", "binds"]]
    if all("vinstr" in r for r in rows):
        sets += [["tris", "draws", "binds", "vinstr"], ["vinstr", "draws", "binds"]]
    print(f"{'features':22s} {'fit rms':>7s} {'cv rms':>7s} {'cv mae':>7s} {'game-out':>8s} {'over ub':>7s}  coefficients")
    fits = {}
    for cols in sets:
        co = hwfit(rows, cols)
        res = [hw_predict(co, r, cols) - r["active"] for r in bound]
        cv = cross_validate(rows, cols)
        go = []
        for g in ("jak1", "jak2"):
            tr = [r for r in rows if r["game"] != g]
            if len(tr) > len(cols) + 5:
                c2 = hwfit(tr, cols)
                go += [hw_predict(c2, r, cols) - r["active"] for r in bound if r["game"] == g]
        over = [r for r in free if hw_predict(co, r, cols) > r["active"] + 1.0]
        desc = f"{co[0]:.2f} ms" + "".join(
            f" + {c * (1 if k == 'tris' else 1000):.3f} {'ms/1k tris' if k == 'tris' else 'us/' + k}"
            for c, k in zip(co[1:], cols))
        print(f"{'+'.join(cols):22s} {rms(res):7.2f} {rms(cv):7.2f} {sum(map(abs, cv)) / max(len(cv), 1):7.2f} "
              f"{rms(go):8.2f} {100.0 * len(over) / max(len(free), 1):6.0f}%  {desc}")
        fits[tuple(cols)] = (co, rms(cv), cv)
    cols = ["tris", "draws", "binds"]
    co, cvr, cv = fits[tuple(cols)]
    print("\n(cv: leave one session out; game-out: fit on one game, error on the other; over ub: "
          "share of non-bound windows the fit puts more than 1 ms above their active)")
    if a.out:
        out = {"model": "hw", "features": cols, "coef": co, "cv_rms_ms": cvr,
               "windows": len(bound), "folders": sorted(set(r["folder"] for r in rows), key=folder_key),
               "phys_scale": getattr(a, "phys_scale", 1.0)}
        json.dump(out, open(a.out, "w"), indent=1)
        print(f"wrote {a.out}")
    if a.show:
        print("\nthe GPU-bound windows:")
        for r in sorted(bound, key=lambda r: (r["game"], -r["tris"]))[: a.show]:
            print(f"  {r['folder']:10s} {r['game']} {r['tris']:7.0f} tris {r['draws']:5.0f} draws {r['binds']:4.0f} binds "
                  f"{r['frames']:4.0f} fr active {r['active']:5.2f} pred {hw_predict(co, r, cols):5.2f}  {r['levels'][:50]}")


# ---------------------------------------------------------------------------------------------
# pica_bench lines -> per-feature costs (the tests: game/graphics/ctr/ctr_gpu_bench.c, and
# platform/3ds/tools/bench_report.py for the table)
BENCH = re.compile(r"\[bench\] (\S+)(.*)$")


def read_bench(path):
    """the last bench session's tests: {name: {key: value}}, and its begin line"""
    sess = []
    for line in open(path, errors="replace"):
        m = BENCH.search(ANSI.sub("", line))
        if not m:
            continue
        name, kv = m.group(1), dict(re.findall(r"(\w+)=(\S+)", m.group(2)))
        if name == "begin" or not sess:
            sess.append(({}, kv if name == "begin" else {}))
        if name not in ("begin", "end"):
            sess[-1][0][name] = kv
    return sess[-1] if sess else ({}, {})


def cmd_bench(a):
    tests, head = read_bench(a.log)
    if not tests:
        sys.exit("no [bench] lines (config.ini preset = bench; docs/3ds-port/gpu_model.md)")
    if head.get("emulator") == "1":
        print("warning: an emulator session: the times mean nothing")
    upm = float(head.get("upm", 0) or 0)

    def ms(t):
        kv = tests.get(t)
        if not kv or "skipped" in kv:
            return None
        try:
            p3d = float(kv.get("p3d", "nan"))
        except ValueError:
            p3d = float("nan")
        return p3d / upm if upm > 0 and p3d == p3d and p3d > 0 else float(kv.get("wall", "nan"))

    def per(t, scale=1e6, div=1.0):
        """ns per item (B2: draw, B3/B5: vertex with div 3, B4: index, B6: pixel, B7: triangle)"""
        v = ms(t)
        n = float(tests.get(t, {}).get("n", 0) or 0)
        return v * scale / n / div if v is not None and n else None

    def diff(x, y):
        return x - y if x is not None and y is not None else None

    costs, notes = {}, []

    def put(k, v, why):
        if v is not None and v == v:
            costs[k] = max(v, 0.0)
            notes.append(f"{k:10s} {costs[k]:10.2f} ns  (prior {PRIOR[k]:8.2f})  {why}")

    put("draws", per("B2.nostate"), "B2.nostate: a draw of one clipped triangle")
    put("binds", diff(per("B2.rebind_clr"), per("B2.nostate")), "B2.rebind_clr - nostate")
    put("stchg", diff(per("B2.texenv"), per("B2.nostate")), "B2.texenv - nostate")
    put("progsw", diff(per("B2.prog_sep"), per("B2.nostate")), "B2.prog_sep - nostate")
    u = diff(per("B2.unif_f32"), per("B2.nostate"))
    put("unif", u / 4.0 if u is not None else None, "(B2.unif_f32 - nostate) / 4 vectors")
    # B4: indexed triangles without area; the best reuse distance prices an index that hits the cache
    hits = [per(t) for t in tests if t.startswith("B4.cdef.k") and per(t) is not None]
    tri_in = 3.0 * min(hits) if hits else None
    put("tri_in", tri_in, "3 x the cheapest B4.cdef index (cache hit: assembly + cull per triangle)")
    # B3: culled triangles, 3 vertices each: ns per vertex against the program's length
    pts = []
    for t in tests:
        m = re.match(r"B3\.v(\d+)$", t)
        if m and per(t, div=3.0) is not None:
            pts.append((int(m.group(1)), per(t, div=3.0)))
    if len(pts) >= 2:
        co = lsq([[1.0, x] for x, _ in pts], [y for _, y in pts])
        put("vinstr", co[1], f"B3.v* fit slope ({len(pts)} programs)")
        put("vmiss", co[0] - (tri_in or 0.0) / 3.0, "B3.v* fit at 0 instructions - tri_in / 3")
    put("vbufs", diff(per("B5.split", div=3.0), per("B5.short", div=3.0)), "B5.split - short (a second buffer)")
    put("tri_setup", diff(per("B7.px1"), per("B7.zero")), "B7.px1 - zero (setup + ~1 fragment)")
    base = per("B6.d24s8") or per("B6.untex")
    put("px", base, "B6.d24s8 (untextured, depth tested and written)")
    put("px_tex", diff(per("B6.rgba8.1to1"), per("B6.untex")), "B6.rgba8.1to1 - untex")
    put("px_u2", diff(per("B6.unit1.read"), per("B6.unit1.off")), "B6.unit1.read - unit1.off")
    put("px_blend", diff(per("B6.blend"), per("B6.rgba8.1to1")), "B6.blend - rgba8.1to1")
    put("px_z", 0.0 if base is not None else None, "(in px: B6.d24s8 writes depth)")
    # min4: 4 texels per pixel along each axis, no mips: ~128 B of RGBA8 tiles missed per pixel
    m4 = diff(per("B6.rgba8.min4"), per("B6.rgba8.1to1"))
    put("tex_kb16", m4 / (128.0 / 1024.0) if m4 is not None else None,
        "(B6.rgba8.min4 - 1to1) per 128 B of tiles a pixel misses")
    if ms("B1.clear") is not None and ms("B1.transfer") is not None:
        fr = (float(tests["B1.clear"].get("wall", 0)) + float(tests["B1.transfer"].get("wall", 0)) +
              float(tests.get("B1.fence", {}).get("wall", 0))) * 1e6
        put("frame", fr, "B1.clear + transfer + fence (wall)")
        put("copy_rows", float(tests["B1.transfer"].get("wall", 0)) * 1e6 / 50.0,
            "B1.transfer / 50 (an 8-pixel band of the 400)")
    print(f"pica_bench session: new3ds={head.get('new3ds', '?')} perflog={head.get('perflog', '?')} upm={upm:g}")
    for n in notes:
        print("  " + n)
    missing = [k for k in FEATURES if k not in costs]
    if missing:
        print("  not measured (prior kept): " + " ".join(missing))
    if a.out:
        json.dump({"model": "bench", "costs_ns": costs}, open(a.out, "w"), indent=1)
        print(f"wrote {a.out}")


# ---------------------------------------------------------------------------------------------
# host logs ([gpumodel] windows of gk --ctr-gfx with CTR_SOFT_STATS=N)
def read_host(path):
    """windows: {frame, frames, view, buckets: {name: {feature: per frame}}}"""
    wins, view, cur = [], "", None
    for line in open(path, errors="replace"):
        line = ANSI.sub("", line)
        m = re.search(r"gpumodel view (\S+ \S+)", line)
        if m:
            view = m.group(1)
            continue
        if "gpumodel route end" in line:
            view = "(end)"
            continue
        m = re.search(r"\[gpumodel\] win frame (\d+) frames (\d+)", line)
        if m:
            cur = dict(frame=int(m.group(1)), frames=int(m.group(2)), view=view, buckets={})
            continue
        if cur is None:
            continue
        m = re.match(r"\[gpumodel\] b (\S+) (.*)", line.strip())
        if m:
            cur["buckets"][m.group(1)] = {k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)", m.group(2))}
            continue
        if line.strip() == "[gpumodel] end":
            wins.append(cur)
            cur = None
    return wins


def totals(win):
    t = {k: 0.0 for k in FEATURES}
    for b in win["buckets"].values():
        for k in FEATURES:
            t[k] += b.get(k, 0.0)
    t["frame"] = 1.0
    return t


def bucket_group(name):
    n = name.split("@")[0]
    if "/" in n:
        kind = n.split("/")[1]
        if kind.startswith("tie") or kind in ("wind", "etie") or "etie" in kind:
            return "tie"
        if kind.startswith("tfrag"):
            return "tfrag"
        if kind.startswith("shrub"):
            return "shrub"
        if "vista" in kind:
            return "vista"
        return "level-other"
    return n.split("-")[0]


def load_costs(path):
    costs = dict(PRIOR)
    if path and os.path.exists(path):
        d = json.load(open(path))
        costs.update(d.get("costs_ns", {}))
    return costs


def phys_ms(feat, costs):
    return sum(feat.get(k, 0.0) * costs[k] for k in FEATURES) / 1e6


def effective_costs(a):
    """ns per feature: bench costs as measured (--costs), else the priors times the calibration's
    group scales (gpu_model_calib.json)"""
    costs = load_costs(a.costs)
    src = "bench costs " + a.costs if a.costs else "prior costs"
    if not a.costs and a.calib and os.path.exists(a.calib):
        sc = json.load(open(a.calib)).get("phys", {}).get("scales", {})
        for g, fs in GROUPS.items():
            for k in fs:
                costs[k] *= sc.get(g, 1.0)
        src = f"prior costs x the group scales of {os.path.basename(a.calib)}"
    return costs, src


def cmd_predict(a):
    costs, src = effective_costs(a)
    print(f"costs: {src}")
    for path in a.host:
        wins = [w for w in read_host(path) if w["buckets"]]
        if a.skip_title:
            wins = [w for w in wins if w["view"]]
        print(f"== {path}: {len(wins)} windows")
        if not wins:
            continue
        rows = [(w, totals(w)) for w in wins]
        for w, t in rows:
            w["ms"] = phys_ms(t, costs)
        if a.windows:
            print(f"{'frame':>6s} {'view':24s} {'k tris':>6s} {'draws':>6s} {'binds':>5s} {'kverts':>6s} "
                  f"{'scr px':>6s} {'GPU ms':>6s}  by group")
            for w, t in rows:
                g = group_ms(t, costs)
                print(f"{w['frame']:6d} {w['view'][:24]:24s} {t['tri_in'] / 1e3:6.1f} {t['draws']:6.0f} "
                      f"{t['binds']:5.0f} {t['vmiss'] / 1e3:6.1f} {t['px'] / 96000:6.2f} {w['ms']:6.1f}  "
                      + " ".join(f"{k} {v:4.1f}" for k, v in g.items()))
        views = {}
        for w, t in rows:
            views.setdefault(w["view"] or "(before the route)", []).append((w, t))
        print(f"\n{'view':28s} {'win':>3s} {'k tris':>6s} {'draws':>5s} {'binds':>5s} {'scr px':>6s} "
              f"{'GPU ms':>6s} {'worst':>6s}  by group (mean)")
        for v, rs in views.items():
            n = len(rs)
            mean = {k: sum(t[k] for _, t in rs) / n for k in FEATURES}
            g = group_ms(mean, costs)
            print(f"{v[:28]:28s} {n:3d} {mean['tri_in'] / 1e3:6.1f} {mean['draws']:5.0f} {mean['binds']:5.0f} "
                  f"{mean['px'] / 96000:6.2f} {sum(w['ms'] for w, _ in rs) / n:6.1f} "
                  f"{max(w['ms'] for w, _ in rs):6.1f}  " + " ".join(f"{k} {x:.1f}" for k, x in g.items()))
        w, t = max(rows, key=lambda r: r[0]["ms"])
        print(f"\nworst window: frame {w['frame']}, view {w['view']}: {w['ms']:.1f} ms (budget 33.3 at 30 fps)")
        print("  by feature: " + ", ".join(
            f"{k} {t[k] * costs[k] / 1e6:.2f}" for k in sorted(FEATURES, key=lambda k: -t[k] * costs[k])
            if t[k] * costs[k] > 5e4))
        bt = []
        for name, b in w["buckets"].items():
            bt.append((sum(b.get(k, 0.0) * costs[k] for k in FEATURES if k != "frame") / 1e6, name, b))
        agg = {}
        for ms, name, f in bt:
            agg[bucket_group(name)] = agg.get(bucket_group(name), 0.0) + ms
        print("  by renderer: " + ", ".join(f"{k} {v:.2f}" for k, v in sorted(agg.items(), key=lambda x: -x[1])))
        print("  top buckets: " + ", ".join(
            f"{n} {ms:.2f} ({f.get('tri_in', 0) / 1e3:.1f}k tris, {f.get('px', 0) / 96000:.2f} scr)"
            for ms, n, f in sorted(bt, key=lambda x: -x[0])[:8]))


# ---------------------------------------------------------------------------------------------
# calibration of the physical model against hardware windows, through host views of the same places
#
# A hardware window has triangles, draws and binds; the host has every feature for views of the same
# levels. Each hardware window gets the host features of its class (its loaded levels -> a host
# view), the triangle-driven ones scaled by its triangles, draws and binds its own. Then one scale
# per feature group is fitted: least squares on the GPU-bound windows, the others as upper bounds.
CLASSES = [  # (game, substring of the window's loaded levels, host view prefix)
    ("jak1", "jungle", "jungle-start"),
    ("jak1", "beach", "beach-start"),
    ("jak1", "village1", "village1-hut"),
    ("jak2", "ctyport", "ctyport-start"),
    ("jak2", "ctysluma", "ctysluma-start"),
    ("jak2", "ctyslumb", "ctyslumb-start"),
    ("jak2", "ctyslumc", "ctyslumb-start"),
]
FIT_GROUPS = ["commands", "vertex", "triangles", "fill"]
PER_TRI = ["tri_setup", "vmiss", "vinstr", "vbufs"]
PER_DRAW = ["stchg", "progsw", "unif"]
FILL = ["px", "px_tex", "px_u2", "px_z", "px_blend", "tex_kb16", "copy_rows"]


def host_classes(paths):
    """per host view prefix: its windows' feature totals"""
    out = {}
    for p in paths:
        for w in read_host(p):
            if not w["view"] or w["view"] == "(end)" or not w["buckets"]:
                continue
            out.setdefault(w["view"].split()[0], []).append(totals(w))
    return out


def window_class(r):
    for game, sub, view in CLASSES:
        if r["game"] == game and sub in r["levels"].split():
            return view
    return None


def impute(r, hc):
    """host features for a hardware window of class hc (a list of host window totals)"""
    tris = r["tris"]
    f = {k: 0.0 for k in FEATURES}
    f["frame"] = 1.0
    f["tri_in"] = tris
    f["draws"], f["binds"] = r["draws"], r["binds"]
    st = sum(h["tri_in"] for h in hc)
    sd = sum(h["draws"] for h in hc)
    for k in PER_TRI:
        f[k] = tris * sum(h[k] for h in hc) / st
    for k in PER_DRAW:
        f[k] = r["draws"] * sum(h[k] for h in hc) / sd
    # fill: the class's heavier half of views (a GPU-bound window is a heavy one)
    hs = sorted(hc, key=lambda h: phys_ms(h, PRIOR))[len(hc) // 2:]
    for k in FILL:
        f[k] = sum(h[k] for h in hs) / len(hs)
    if "vinstr" in r:  # logged by the hardware itself (gpumodel hw line)
        f["vinstr"] = r["vinstr"]
    return f


def group_ms(f, costs, groups=GROUPS):
    return {g: sum(f.get(k, 0.0) * costs[k] for k in fs) / 1e6 for g, fs in groups.items()}


def scaled_ms(f, costs, scales):
    g = group_ms(f, costs)
    return g["frame"] + sum(scales.get(k, 1.0) * g[k] for k in FIT_GROUPS)


def fit_scales(rows, costs, fit=("commands", "vertex", "triangles", "fill")):
    """group scales: least squares on GPU-bound windows, hinge on the upper bounds"""
    fit = list(fit)
    bound = [r for r in rows if r["gpu_bound"]]
    free = [r for r in rows if not r["gpu_bound"]]

    def design(r):
        g = group_ms(r["f"], costs)
        fixed = g["frame"] + sum(g[k] for k in FIT_GROUPS if k not in fit)
        return [g[k] for k in fit], r["active"] - fixed

    # each place and session weighs the same in all (a static view logs many equal windows)
    cnt = {}
    for r in bound:
        cnt[(r["cls"], r["sess"])] = cnt.get((r["cls"], r["sess"]), 0) + 1
    wb = {id(r): 1.0 / cnt[(r["cls"], r["sess"])] for r in bound}
    used = set()
    co = [1.0] * len(fit)
    for _ in range(50):
        rs = bound + [free[i] for i in sorted(used)]
        X, y = zip(*[design(r) for r in rs])
        w = [wb.get(id(r), 0.25) for r in rs]
        # ridge towards the prior scale 1 (unidentified groups stay near the first-principles cost)
        lam = 0.02 * sum(wi * sum(x * x for x in row) for row, wi in zip(X, w)) / sum(w)
        M = [[sum(wi * x[i] * x[j] for x, wi in zip(X, w)) + (lam if i == j else 0.0) for j in range(len(fit))] for i in range(len(fit))]
        v = [sum(wi * x[i] * yy for x, yy, wi in zip(X, y, w)) + lam for i in range(len(fit))]
        co = [max(c, 0.05) for c in solve(M, v)]
        sc = dict(zip(fit, co))
        viol = {i for i, r in enumerate(free) if scaled_ms(r["f"], costs, sc) > r["active"]}
        if used | viol == used:
            break
        used |= viol
    return dict(zip(fit, co))


def cmd_calibrate(a):
    costs = load_costs(a.costs)
    hc = host_classes(a.host)
    rows = read_hw(a.logs)
    if a.since:
        rows = [r for r in rows if folder_key(r["folder"]) >= folder_key(a.since)]
    use = []
    for r in rows:
        if r.get("pools_kb", 0) > 0 and r.get("pools_vram", 1) == 0:
            continue  # level textures in FCRAM (vram_textures off or no room): another cost regime
        c = window_class(r)
        if c and c in hc:
            r["cls"] = c
            r["f"] = impute(r, hc[c])
            use.append(r)
    bound = [r for r in use if r["gpu_bound"]]
    print(f"host views: {', '.join(f'{k} ({len(v)} windows)' for k, v in hc.items())}")
    print(f"hardware windows of those places: {len(use)}, {len(bound)} GPU-bound "
          f"({', '.join(sorted(set(r['cls'] for r in bound)))})\n")
    if len(bound) < 4:
        sys.exit("too few GPU-bound windows")
    variants = [("prior costs", None), ("one scale", ("commands", "vertex", "triangles", "fill"))]
    results = {}
    for name, fit in variants:
        if fit is None:
            sc = {}
        elif name == "one scale":
            # one common factor: fit as a single group
            X = [sum(v for k, v in group_ms(r["f"], costs).items() if k != "frame") for r in bound]
            y = [r["active"] - group_ms(r["f"], costs)["frame"] for r in bound]
            k = sum(x * yy for x, yy in zip(X, y)) / sum(x * x for x in X)
            sc = {g: k for g in FIT_GROUPS}
        results[name] = sc
    results["group scales"] = fit_scales(use, costs)

    def cv_err(fitter, key):
        errs = []
        for c in sorted(set(r[key] for r in bound), key=str):
            tr = [r for r in use if r[key] != c]
            if sum(r["gpu_bound"] for r in tr) < 4:
                continue
            sc = fitter(tr)
            errs += [scaled_ms(r["f"], costs, sc) - r["active"] for r in bound if r[key] == c]
        return errs

    def one_scale(tr):
        b = [r for r in tr if r["gpu_bound"]]
        X = [sum(v for k, v in group_ms(r["f"], costs).items() if k != "frame") for r in b]
        y = [r["active"] - group_ms(r["f"], costs)["frame"] for r in b]
        k = sum(x * yy for x, yy in zip(X, y)) / sum(x * x for x in X)
        return {g: k for g in FIT_GROUPS}

    fitters = {"prior costs": lambda tr: {}, "one scale": one_scale,
               "group scales": lambda tr: fit_scales(tr, costs)}
    print(f"{'model':14s} {'fit rms':>7s} {'cv sess':>7s} {'cv place':>8s} {'cv game':>7s} {'over ub':>7s}  scales")
    for name, sc in results.items():
        e = [scaled_ms(r["f"], costs, sc) - r["active"] for r in bound]
        free = [r for r in use if not r["gpu_bound"]]
        over = sum(scaled_ms(r["f"], costs, sc) > r["active"] + 1.0 for r in free)
        cs = cv_err(fitters[name], "sess")
        cp = cv_err(fitters[name], "cls")
        cg = cv_err(fitters[name], "game")
        print(f"{name:14s} {rms(e):7.2f} {rms(cs):7.2f} {rms(cp):8.2f} {rms(cg):7.2f} "
              f"{100.0 * over / max(len(free), 1):6.0f}%  " + " ".join(f"{k} {v:.2f}" for k, v in sc.items()))
    print("\n(cv sess / place / game: leave one session / place / game out, error on its GPU-bound "
          "windows; over ub: non-bound windows predicted > 1 ms above their active)")
    best = a.model
    sc = results[best]
    print(f"\nper-unit costs of '{best}' (prior x group scale):")
    for g in FIT_GROUPS:
        print(f"  {g:10s} x{sc.get(g, 1.0):.2f}: " + ", ".join(
            f"{k} {costs[k] * sc.get(g, 1.0):.1f} ns" for k in GROUPS[g]))
    print(f"\nGPU-bound windows, model '{best}':")
    for r in sorted(bound, key=lambda r: (r["game"], r["cls"], -r["tris"])):
        g = group_ms(r["f"], costs)
        print(f"  {r['folder']:10s} {r['cls']:15s} {r['tris'] / 1e3:6.1f}k tris {r['draws']:5.0f} dr {r['binds']:4.0f} bi "
              f"measured {r['active']:5.1f}{'*' if r['cpu_near'] else ' '} predicted {scaled_ms(r['f'], costs, sc):5.1f}")
    print("  (* the render thread's own work was >= 85% of the frame period: the measured time may "
          "include waits for its partial submits)")
    # validation: every window of the newest folders, the scales fitted without its game
    print(f"\nvalidation, windows of {a.validate} (scales fitted without the window's game; active is the "
          f"GPU time when GPU-bound, else only an upper bound):")
    print(f"  {'game':5s} {'place':15s} {'n':>3s} {'bound':>5s} {'k tris':>6s} {'draws':>5s} {'binds':>5s} "
          f"{'active':>6s} {'predicted':>9s}  {'bound err':>9s}")
    for game in ("jak1", "jak2"):
        sc_out = fit_scales([r for r in use if r["game"] != game], costs) if a.model == "group scales" else sc
        vs = [r for r in use if r["game"] == game and r["folder"] in a.validate.split(",")]
        for c in sorted(set(r["cls"] for r in vs)):
            rs = [r for r in vs if r["cls"] == c]
            n = len(rs)
            pr = [scaled_ms(r["f"], costs, sc_out) for r in rs]
            be = [p - r["active"] for p, r in zip(pr, rs) if r["gpu_bound"]]
            print(f"  {game:5s} {c:15s} {n:3d} {len(be):5d} {sum(r['tris'] for r in rs) / n / 1e3:6.1f} "
                  f"{sum(r['draws'] for r in rs) / n:5.0f} {sum(r['binds'] for r in rs) / n:5.0f} "
                  f"{sum(r['active'] for r in rs) / n:6.1f} {sum(pr) / n:9.1f}  "
                  f"{(f'{sum(be) / len(be):+.1f}' if be else ''):>9s}")
    if a.out:
        d = json.load(open(a.out)) if os.path.exists(a.out) else {}
        d.update({"phys": {"scales": sc, "model": best, "costs_ns": costs,
                           "windows": len(use), "bound": len(bound)}})
        json.dump(d, open(a.out, "w"), indent=1)
        print(f"wrote {a.out}")


def cmd_draws(a):
    import csv
    costs, _ = effective_costs(a)
    rows = list(csv.DictReader(open(a.csv)))
    if not rows:
        sys.exit("empty")
    frames = sorted(set(int(r["frame"]) for r in rows))
    fr = int(a.frame) if a.frame else frames[len(frames) // 2]
    rs = [r for r in rows if int(r["frame"]) == fr]
    out = []
    for r in rs:
        f = {"draws": 1, "binds": int(r["bind"]), "stchg": int(r["stchg"]), "tri_in": float(r["tri_in"]),
             "tri_setup": float(r["tri_setup"]), "vmiss": float(r["vmiss"]),
             "vinstr": float(r["vmiss"]) * int(r["instr"]), "vbufs": float(r["vmiss"]) * int(r["bufs"]),
             "px": float(r["px"]), "px_tex": float(r["px"]) if int(r["tex"]) >= 0 else 0.0,
             "px_u2": float(r["px_u2"]), "px_z": float(r["px_z"]),
             "px_blend": float(r["px_z"]) if int(r["blend"]) else 0.0}
        out.append((phys_ms(f, costs) * 1000, r))
    out.sort(key=lambda x: -x[0])
    tot = sum(x[0] for x in out)
    print(f"frame {fr}: {len(rs)} draws, model {tot / 1000:.2f} ms (without the per-frame cost)")
    for us, r in out[: a.top]:
        print(f"  {us:7.1f} us {r['bucket']:22s} prog {r['prog']:>2s} instr {r['instr']:>2s} tris {r['tri_in']:>5s} "
              f"setup {r['tri_setup']:>5s} px {r['px']:>6s} tex {r['tex']:>4s} {r['tw']}x{r['th']} fmt {r['fmt']} blend {r['blend']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    h = sp.add_parser("hwfit")
    h.add_argument("logs", nargs="+", help="gk.log files or report folders")
    h.add_argument("--since", help="first report folder of the fit, e.g. hw-nov10b")
    h.add_argument("--out")
    h.add_argument("--show", type=int, default=0, help="list the N heaviest windows")
    b = sp.add_parser("bench")
    b.add_argument("log")
    b.add_argument("--out")
    q = sp.add_parser("predict")
    q.add_argument("host", nargs="+")
    q.add_argument("--calib", default=DEFAULT_CALIB)
    q.add_argument("--costs")
    q.add_argument("--skip-title", action="store_true", help="only windows after the first view mark")
    q.add_argument("--windows", action="store_true", help="a line per window too")
    c = sp.add_parser("calibrate")
    c.add_argument("--hw", dest="logs", nargs="+", required=True)
    c.add_argument("--host", nargs="+", required=True)
    c.add_argument("--since", default="hw-nov10b")
    c.add_argument("--costs")
    c.add_argument("--validate", default="hw-nov11,hw-nov11b", help="report folders to validate on")
    c.add_argument("--model", default="group scales", choices=["prior costs", "one scale", "group scales"])
    c.add_argument("--out")
    d = sp.add_parser("draws")
    d.add_argument("csv")
    d.add_argument("--costs")
    d.add_argument("--calib", default=DEFAULT_CALIB)
    d.add_argument("--frame")
    d.add_argument("--top", type=int, default=25)
    a = p.parse_args()
    {"hwfit": cmd_hwfit, "bench": cmd_bench, "predict": cmd_predict, "draws": cmd_draws,
     "calibrate": cmd_calibrate}[a.cmd](a)


if __name__ == "__main__":
    main()
