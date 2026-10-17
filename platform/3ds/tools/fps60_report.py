#!/usr/bin/env python3
# (AI-assisted)
"""Per checkpoint frame rate table from a gk.log of the 60 fps sweep (platform/3ds/tests/
fps60_sweep.pad with config.ini fps_cap = 60 and stats_period = 5).

    fps60_report.py gk.log [--azahar azahar.log] [--md]

For each "fps60 N NAME" marker it takes the stats windows that start after it (the first
`[ctr] game thread ms/frame` line after the marker is skipped: that window began before the
marker) up to the get-current-time call that ends the checkpoint, and prints:
  fps mean (frame weighted) / min window, logic ms (game thread), wait render (game thread waits
  for the render thread), uploads ms, render build + submit ms and wait-gpu (render thread),
  triangles and draws per frame, GPU busy ms (the fence: on hardware the GPU's time per frame;
  in Azahar, whose GPU takes no time, it only follows the render thread's build), the GPU
  estimate 10.6 ms + 0.13 ms per 1k triangles (hardware fit), the game time rate (game time
  over real time, from get-current-time and the frames between the two calls: 1.00 = right;
  below 60 fps it falls to ~fps/60: display-frame-start rounds the time ratio of a 17-25 ms
  frame to 1, so the game runs in slow motion), Jak's state at the end, and what limits the
  frame rate.
--azahar: also the emulation speed (the game's log clock over Azahar's host clock).
"""
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TS = re.compile(r"^\[(\d+):(\d+)\]")
MARK = re.compile(r"\[pad script\] frame (\d+): fps60 (\d+) (\S+)")
CP = re.compile(r"\[pad script\] frame (\d+): CP (\d+) (\S+)")
CALLF = re.compile(r"\[pad script\] frame (\d+): call get-current-time")
CALLR = re.compile(r"\[pad script\] call get-current-time = \S+ \((-?\d+)\)")
POS = re.compile(r"\[pad script\] frame \d+: \*target\* at .* state (\S+)")
GAME = re.compile(
    r"\[ctr\] game thread ms/frame over (\d+) frames: frame ([\d.]+) \(([\d.]+) fps\) = "
    r"logic (-?[\d.]+) \+ sync render (-?[\d.]+) \+ wait render thread (-?[\d.]+) \+ vsync "
    r"(-?[\d.]+) \+ texture uploads (-?[\d.]+).*longest frame (\d+) ms")
REND = re.compile(
    r"\[ctr\] render ms/frame over (\d+) frames: wait-gpu ([\d.]+), build ([\d.]+), submit "
    r"([\d.]+).*?gpu ([\d.]+) \(draw ([\d.]+), busy ([\d.]+).*?\)\); (\d+) triangles, (\d+) draws")
PERF = re.compile(r"\] perf: ([\d.]+) fps")
CPU = re.compile(r"3DS hardware: cpu ~(\d+) MHz")
LOADED = re.compile(r"\[ctr\] (loaded|unloading) ")
BUDGET = 1000.0 / 60


def gpu_est(tris):
    return 10.6 + 0.13 * tris / 1000.0


def parse(path):
    cps = []  # dicts
    cur = None  # checkpoint being measured
    state = "out"  # out | skip (marker seen, first window to skip) | in | done
    last_rend = None
    cpu = None
    pending_call_frame = None
    for raw in open(path, errors="replace"):
        line = ANSI.sub("", raw.rstrip("\n"))
        m = CPU.search(line)
        if m:
            cpu = int(m.group(1))
        m = CP.search(line)
        if m:
            if cur is not None:
                cps.append(cur)
            cur = {"n": int(m.group(2)), "name": m.group(3), "win": [], "calls": [],
                   "state": "?", "loads": 0, "skipped": None, "marker": False}
            state = "out"
            continue
        if cur is None:
            continue
        if LOADED.search(line) and state in ("skip", "in"):
            cur["loads"] += 1
        m = MARK.search(line)
        if m:
            cur["marker"] = True
            state = "skip"
            continue
        m = CALLF.search(line)
        if m:
            pending_call_frame = int(m.group(1))
            continue
        m = CALLR.search(line)
        if m and pending_call_frame is not None:
            cur["calls"].append((pending_call_frame, int(m.group(1))))
            pending_call_frame = None
            if state in ("skip", "in") and len(cur["calls"]) >= 2:
                state = "done"
            continue
        m = POS.search(line)
        if m:
            cur["state"] = m.group(1).replace("target-", "")
            continue
        m = REND.search(line)
        if m:
            last_rend = {"frames": int(m.group(1)), "wait_gpu": float(m.group(2)),
                         "build": float(m.group(3)), "submit": float(m.group(4)),
                         "busy": float(m.group(7)), "tris": int(m.group(8)),
                         "draws": int(m.group(9))}
            continue
        m = GAME.search(line)
        if m:
            w = {"frames": int(m.group(1)), "frame_ms": float(m.group(2)),
                 "fps": float(m.group(3)), "logic": float(m.group(4)),
                 "sync": float(m.group(5)), "wait_render": float(m.group(6)),
                 "vsync": float(m.group(7)), "uploads": float(m.group(8)),
                 "longest": int(m.group(9)), "rend": last_rend, "perf_fps": None}
            last_rend = None
            if state == "skip":
                cur["skipped"] = w
                state = "in"
            elif state == "in":
                cur["win"].append(w)
            continue
        m = PERF.search(line)
        if m and state == "in" and cur["win"] and cur["win"][-1]["perf_fps"] is None:
            cur["win"][-1]["perf_fps"] = float(m.group(1))
    if cur is not None:
        cps.append(cur)
    return cps, cpu


def avg(ws, f, weight="frames"):
    tot = sum(w[weight] for w in ws)
    return sum(f(w) * w[weight] for w in ws) / tot if tot else float("nan")


def summarize(c):
    ws = c["win"]
    if not ws:
        return None
    frames = sum(w["frames"] for w in ws)
    fps = frames / (sum(w["frames"] * w["frame_ms"] for w in ws) / 1000.0)
    r = {"fps": fps, "min": min(w["fps"] for w in ws), "n": len(ws),
         "logic": avg(ws, lambda w: w["logic"]),
         "wait_render": avg(ws, lambda w: w["wait_render"] + w["sync"]),
         "uploads": avg(ws, lambda w: w["uploads"]),
         "longest": max(w["longest"] for w in ws)}
    rs = [w["rend"] for w in ws if w["rend"]]
    if rs:
        r["build"] = avg(rs, lambda x: x["build"] + x["submit"])
        r["wait_gpu"] = avg(rs, lambda x: x["wait_gpu"])
        r["tris"] = avg(rs, lambda x: x["tris"])
        r["draws"] = avg(rs, lambda x: x["draws"])
        r["busy"] = avg(rs, lambda x: x["busy"])
    else:
        r.update(build=float("nan"), wait_gpu=float("nan"), tris=0, draws=0, busy=float("nan"))
    r["gpu_est"] = gpu_est(r["tris"])
    # game time over real time: ticks (1/300 s) between the two calls over the frames between
    # them at the measured frame rate
    r["rate"] = float("nan")
    if len(c["calls"]) >= 2:
        (f0, t0), (f1, t1) = c["calls"][0], c["calls"][-1]
        if f1 > f0:
            r["rate"] = ((t1 - t0) / 300.0) / ((f1 - f0) / fps)
    # what limits it: the game thread's own work, the render thread's, or (both under budget)
    # the vblank schedule / GPU
    if fps >= 58:
        lim = "-"
    else:
        game = r["logic"] + r["uploads"]
        rend = r["build"] if r["build"] == r["build"] else 0
        if game >= rend and game > 0.8 * BUDGET:
            lim = "logic"
        elif rend > game and rend > 0.8 * BUDGET:
            lim = "render thread"
        elif r["busy"] == r["busy"] and r["busy"] > 0.8 * BUDGET:
            lim = "GPU"
        else:
            lim = "mixed/pacing"
        if r["uploads"] > 2:
            lim += " +uploads"
    r["lim"] = lim
    return r


def emu_speed(path):
    """game log clock (mm:ss) against Azahar's host clock, over the stats lines"""
    pts = []
    rx = re.compile(r"^\[\s*([\d.]+)\].*OutputDebugString:\d+: \[(\d+):(\d+)\] .*game thread ms/frame")
    for raw in open(path, errors="replace"):
        m = rx.search(ANSI.sub("", raw))
        if m:
            pts.append((float(m.group(1)), int(m.group(2)) * 60 + int(m.group(3))))
    if len(pts) < 3:
        return None
    # unwrap the hour
    off, prev, g = 0, pts[0][1], []
    for h, s in pts:
        if s + off < prev - 1800:
            off += 3600
        prev = s + off
        g.append((h, s + off))
    (h0, s0), (h1, s1) = g[1], g[-1]
    return (s1 - s0) / (h1 - h0) if h1 > h0 else None


def main():
    args = sys.argv[1:]
    md = "--md" in args
    az = None
    if "--azahar" in args:
        az = args[args.index("--azahar") + 1]
    paths = [a for a in args if not a.startswith("--") and a != az]
    if not paths:
        print(__doc__)
        return 1
    cps, cpu = parse(paths[0])
    if cpu:
        print(f"3DS hardware line: cpu ~{cpu} MHz (804 = New 3DS at full clock; Azahar: ~268)")
    if az:
        sp = emu_speed(az)
        print("emulation speed: " + (f"{sp * 100:.0f}% (game log clock / host clock)" if sp else "n/a"))
    hdr = ["CP", "checkpoint", "fps mean", "fps min", "win", "logic ms", "wait rend", "upl ms",
           "build ms", "wait-gpu", "ktris", "draws", "gpu busy", "gpu est", "est fps", "time rate",
           "state", "limit"]
    rows = []
    held = 0
    measured = 0
    for c in cps:
        r = summarize(c)
        if r is None:
            rows.append([str(c["n"]), c["name"]] + ["-"] * (len(hdr) - 4)
                        + [c["state"], "no window" if c["marker"] else "no marker"])
            continue
        measured += 1
        if r["fps"] >= 58:
            held += 1
        est_fps = min(60.0, 1000.0 / r["gpu_est"])
        rows.append([str(c["n"]), c["name"], f"{r['fps']:.1f}", f"{r['min']:.1f}", str(r["n"]),
                     f"{r['logic']:.1f}", f"{r['wait_render']:.1f}", f"{r['uploads']:.1f}",
                     f"{r['build']:.1f}", f"{r['wait_gpu']:.1f}", f"{r['tris'] / 1000:.1f}",
                     f"{r['draws']:.0f}", f"{r['busy']:.1f}", f"{r['gpu_est']:.1f}",
                     f"{est_fps:.0f}", f"{r['rate']:.2f}", c["state"],
                     r["lim"] + (f" (load x{c['loads']})" if c["loads"] else "")])
    if md:
        print("| " + " | ".join(hdr) + " |")
        print("|" + "---|" * len(hdr))
        for row in rows:
            print("| " + " | ".join(row) + " |")
    else:
        wid = [max(len(h), *(len(r[i]) for r in rows)) for i, h in enumerate(hdr)]
        print("  ".join(h.ljust(wid[i]) for i, h in enumerate(hdr)))
        for row in rows:
            print("  ".join(v.ljust(wid[i]) for i, v in enumerate(row)))
    print(f"\n{held} of {measured} measured checkpoints hold >= 58 fps "
          f"({len(cps)} checkpoints in the log)")
    print("gpu est = 10.6 ms + 0.13 ms per 1k triangles (hardware fit; 60 fps needs <= 16.7 ms); "
          "time rate = game time / real time (1.00 = right speed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
