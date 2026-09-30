#!/usr/bin/env python3
# (AI-assisted)
# Summarize the frame timing of a gk run (stdout.log) per measurement window.
#
#   perf_report.py stdout.log [--top N] [--window NAME] [--compare other_stdout.log]
#
# Windows start at "perf-mark NAME" lines (platform/3ds/tests/perf.pad) and end at the next mark.
# For each window: fps and "logic" from the kperf lines (once a second), the renderer's game thread
# and render thread lines, the other threads on the game's core, and (if the run had
# --perf-sections) the average ms/frame of every section. The first second of a window is left
# out (it straddles the previous window).
import argparse
import collections
import re
import statistics

PERF = re.compile(r"perf: +([\d.]+) fps +logic +([-\d.]+) +render +([-\d.]+) vsync +([-\d.]+) +idle +([-\d.]+)")
SECS = re.compile(r"perf sections \(ms/frame[^)]*\)?\)?: (.*)")
THREADS = re.compile(r"perf threads \(ms/frame\): iop ([\d.]+) \(([\d.]+) dispatches/s\), sound ([\d.]+) \(([\d.]+)/s(?:, (\d+) handlers)?\); kernel: ([\d.]+) thread suspends/frame, ([\d.]+) KB")
NSECS = re.compile(r"perf sections: (\d+) timed sections per frame")
GAME = re.compile(r"game thread ms/frame over (\d+) frames: frame ([\d.]+).*logic ([-\d.]+) \+ sync render ([\d.]+) \+ wait render thread ([\d.]+) \+ vsync ([\d.]+) \+ texture uploads ([\d.]+).*bone snapshot ([\d.]+)")
REND = re.compile(r"render ms/frame over (\d+) frames: wait-gpu ([\d.]+), build ([\d.]+), submit ([\d.]+) \(cpu total ([\d.]+)\); gpu ([\d.]+)")
BUILD = re.compile(r"build ms/frame by renderer: (.*)")
MARK = re.compile(r"perf-mark (\S+)")
SEC_ITEM = re.compile(r"^(.*?) ([\d.]+)(?: \(([\d.]+)\))?$")


def mean(v):
    return sum(v) / len(v) if v else 0.0


def parse(path):
    windows = collections.OrderedDict()
    cur = None
    skip = 0
    for line in open(path, errors="replace"):
        m = MARK.search(line)
        if m:
            cur = {"perf": [], "secs": [], "threads": [], "nsecs": [], "game": [], "rend": [], "build": []}
            windows[m.group(1)] = cur
            skip = 1
            continue
        if cur is None:
            continue
        m = PERF.search(line)
        if m:
            if skip:
                skip -= 1
                cur["_skipping"] = True
                continue
            cur["_skipping"] = False
            cur["perf"].append([float(x) for x in m.groups()])
            continue
        if cur.get("_skipping", True):
            continue  # lines that belong to a skipped perf line
        m = SECS.search(line)
        if m:
            d = {}
            for part in m.group(1).split(", "):
                mm = SEC_ITEM.match(part)
                if mm:
                    d[mm.group(1)] = (float(mm.group(2)), float(mm.group(3) or 0))
            cur["secs"].append(d)
            continue
        m = THREADS.search(line)
        if m:
            cur["threads"].append([float(x or -1) for x in m.groups()])
            continue
        m = NSECS.search(line)
        if m:
            cur["nsecs"].append(float(m.group(1)))
            continue
        m = GAME.search(line)
        if m:
            cur["game"].append([float(x) for x in m.groups()])
            continue
        m = REND.search(line)
        if m:
            cur["rend"].append([float(x) for x in m.groups()])
            continue
        m = BUILD.search(line)
        if m:
            p = m.group(1).split()
            cur["build"].append({p[i]: float(p[i + 1]) for i in range(0, len(p) - 1, 2)})
    return windows


def summary(name, w, top):
    out = []
    perf = w["perf"]
    if not perf:
        return [f"== {name}: no perf lines"]
    fps = [p[0] for p in perf]
    logic = [p[1] for p in perf]
    sd = statistics.pstdev(logic) if len(logic) > 1 else 0
    out.append(f"== {name}: {len(perf)} s, fps {mean(fps):.1f}, logic {mean(logic):.1f} ms "
               f"(sd {sd:.1f}, {min(logic):.1f}-{max(logic):.1f}), render {mean([p[2] for p in perf]):.1f}, "
               f"vsync {mean([p[3] for p in perf]):.1f}, idle {mean([p[4] for p in perf]):.1f}")
    if w["game"]:
        g = w["game"]
        out.append("   game thread: frame %.1f = logic %.1f + wait render thread %.1f + vsync %.1f + tex uploads %.2f + bones %.2f" %
                   tuple(mean([x[i] for x in g]) for i in (1, 2, 4, 5, 6, 7)))
    if w["rend"]:
        r = w["rend"]
        out.append("   render thread: cpu %.1f (wait-gpu %.1f build %.1f submit %.2f), gpu %.1f" %
                   tuple(mean([x[i] for x in r]) for i in (4, 1, 2, 3, 5)))
    if w["build"]:
        keys = collections.OrderedDict()
        for d in w["build"]:
            for k in d:
                keys[k] = 1
        out.append("   render build: " + " ".join(f"{k} {mean([d.get(k, 0) for d in w['build']]):.2f}" for k in keys))
    if w["threads"]:
        t = w["threads"]
        out.append("   other threads on core 0: iop %.2f ms/frame (%.0f dispatches/s), sound %.2f ms/frame (%.0f/s, %.0f handlers at the end); %.0f suspends/frame, %.1f KB stack saved/frame" %
                   (mean([x[0] for x in t]), mean([x[1] for x in t]), mean([x[2] for x in t]), mean([x[3] for x in t]), t[-1][4],
                    mean([x[5] for x in t]), mean([x[6] for x in t])))
    if w["secs"]:
        tot = collections.defaultdict(float)
        calls = collections.defaultdict(float)
        for d in w["secs"]:
            for k, (ms, c) in d.items():
                tot[k] += ms
                calls[k] += c
        n = len(w["secs"])
        extra = f", {mean(w['nsecs']):.0f} timed sections/frame" if w["nsecs"] else ""
        out.append(f"   sections ({n} lines{extra}; inclusive ms/frame, calls/frame):")
        for k, v in sorted(tot.items(), key=lambda kv: -kv[1])[:top]:
            c = calls[k] / n
            out.append(f"     {v / n:7.2f}  {c:7.1f}  {k}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--window", action="append")
    a = ap.parse_args()
    windows = parse(a.log)
    for name, w in windows.items():
        if a.window and name not in a.window:
            continue
        print("\n".join(summary(name, w, a.top)))


if __name__ == "__main__":
    main()
