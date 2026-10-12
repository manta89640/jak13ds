#!/usr/bin/env python3
# (AI-assisted) Table of a config.ini pica_bench run (game/graphics/ctr/ctr_gpu_bench.c).
#   bench_report.py <gk.log | stdout.log | bench.txt> [more files] [--all]
# Reads the `[bench] ...` lines of the last bench session (--all: every session, one after the
# other) and prints, per test group, the GPU time and what it means per item, the hotspot stage
# shares and the memory traffic (calibrated with B1's fills and transfers). docs/3ds-port/
# performance.md "GPU timing in one session" explains the columns.
import re
import sys

STAGES = ["arrays", "vsh", "setup", "raster", "tex", "light", "comb", "fb"]
TRAFFIC = ["main-r", "main-w", "vramA-r", "vramA-w", "vramB-r", "vramB-w", "arrays", "tex",
           "depth-r", "depth-w", "color-r", "color-w", "lcd-top", "lcd-bot", "memcopy-r",
           "memcopy-w", "fill0-w", "fill1-w", "cpu-vram-r", "cpu-vram-w"]


def parse_line(text):
    m = re.search(r"\[bench\] (\S+)(.*)$", text)
    if not m:
        return None
    test, rest = m.group(1), m.group(2)
    kv = {}
    for k, v in re.findall(r"(\w+)=(\S+)", rest):
        kv[k] = v
    return test, kv, rest.strip()


def sessions(paths):
    out, cur = [], None
    for p in paths:
        for raw in open(p, encoding="utf-8", errors="replace"):
            m = re.match(r"start (\S+)", raw)
            if m and cur is not None:
                cur["last_start"] = m.group(1)  # bench.txt: the test about to run
                continue
            if "[bench] " not in raw:
                continue
            r = parse_line(raw)
            if not r:
                continue
            test, kv, rest = r
            if test == "begin":
                cur = {"head": kv, "tests": [], "end": None, "file": p}
                out.append(cur)
                continue
            if cur is None:
                cur = {"head": {}, "tests": [], "end": None, "file": p}
                out.append(cur)
            if test == "end":
                cur["end"] = kv
                continue
            cur["tests"].append((test, kv, rest))
    return out


def num(kv, key, default=None):
    try:
        return float(kv[key])
    except (KeyError, ValueError):
        return default


def traffic(kv):
    t = {}
    v = kv.get("tr", "-")
    if v and v != "-":
        for part in v.split(","):
            i, c = part.split(":")
            t[int(i)] = float(c)
    return t


def shares(kv):
    if "hs" not in kv:
        return ""
    hs = [int(x) for x in kv["hs"].split(",")]
    total = sum(hs)
    if not total:
        return "hs 0"
    top = sorted(range(8), key=lambda i: -hs[i])[:3]
    return " ".join(f"{STAGES[i]} {100.0 * hs[i] / total:.0f}%" for i in top if hs[i]) + \
        f" ({total}/{kv.get('hsx', '?')})"


def report(s):
    head = s["head"]
    upm = num(head, "upm", 0.0) or 0.0
    print(f"== pica_bench ({s['file']}): new3ds={head.get('new3ds', '?')} "
          f"emulator={head.get('emulator', '?')} perflog={head.get('perflog', '?')} "
          f"upm={upm:g} (raw perflog units per ms) hw={head.get('hw', '?')} "
          f"setup={head.get('setup', '?')}")
    if head.get("emulator") == "1":
        print("   (emulator: the times and counters mean nothing, only completion does)")
    tests = {t: (kv, rest) for t, kv, rest in s["tests"]}

    def gpu_ms(kv):
        """the command lists' GPU time per rep (perflog P3D), else the queue's wall time"""
        p3d = num(kv, "p3d")
        if p3d is not None and upm > 0 and p3d > 0:
            return p3d / upm
        return num(kv, "wall", 0.0)

    # traffic calibration: bytes per count from B1's fills and display transfer
    cal = {}
    if "B1.clear" in tests:
        t = traffic(tests["B1.clear"][0])
        for idx, key in ((16, "bytes_fill0"), (17, "bytes_fill1")):
            b = num(tests["B1.clear"][0], key)
            if t.get(idx) and b:
                cal[TRAFFIC[idx]] = b / t[idx]
    if "B1.transfer" in tests:
        t = traffic(tests["B1.transfer"][0])
        for idx, key in ((14, "bytes_r"), (15, "bytes_w")):
            b = num(tests["B1.transfer"][0], key)
            if t.get(idx) and b:
                cal[TRAFFIC[idx]] = b / t[idx]
    unit = None
    if cal:
        print("traffic calibration (bytes per count): " +
              ", ".join(f"{k} {v:.3g}" for k, v in cal.items()))
        vals = sorted(cal.values())
        unit = vals[len(vals) // 2]
    else:
        print("traffic calibration: none (no B1 fill/transfer counts); traffic shown in counts")

    def tr_text(kv, top=4):
        t = traffic(kv)
        if not t:
            return ""
        items = sorted(t.items(), key=lambda x: -x[1])[:top]
        if unit:
            return " ".join(f"{TRAFFIC[i]} {c * unit / 1024:.0f}K" for i, c in items)
        return " ".join(f"{TRAFFIC[i]} {c:.0f}" for i, c in items)

    def row(test, per_label, per_scale, base=None):
        kv, rest = tests[test]
        if "skipped" in kv:
            print(f"  {test:26s} skipped ({kv['skipped']})")
            return None
        ms = gpu_ms(kv)
        n = num(kv, "n", 0) or 0
        per = ms * per_scale / n if n else 0.0
        delta = f" ({per - base:+.3g})" if base is not None else ""
        print(f"  {test:26s} gpu {ms:8.3f} ms  {per:9.4g} {per_label}{delta:12s} wall "
              f"{num(kv, 'wall', 0):7.3f}  {shares(kv):38s} {tr_text(kv)}")
        return per

    groups = [
        ("B1 floor", "B1."),
        ("B2 per draw (2000 clipped one-triangle draws)", "B2."),
        ("B3 vertex shader cost (100k culled triangles)", "B3."),
        ("B4 post-transform vertex cache (300k indices, all triangles without area)", "B4."),
        ("B5 attribute fetch (100k culled triangles, ctr_basic)", "B5."),
        ("B6 fill (20 full screen quads, 1.92 Mpixels)", "B6."),
        ("B7 triangle setup (100k triangles)", "B7."),
        ("B8 early depth (readback census: a = near/small quad, b = full quad, c = clear)",
         "B8."),
    ]
    for title, prefix in groups:
        names = [t for t, _, _ in s["tests"] if t.startswith(prefix)]
        if not names:
            continue
        print(title)
        if prefix == "B1.":
            for t in names:
                kv, rest = tests[t]
                extra = ""
                for k in ("ppf", "psc0", "psc1"):
                    v = num(kv, k)
                    if v and upm:
                        extra += f" {k} {v / upm:.3f} ms"
                print(f"  {t:26s} wall {num(kv, 'wall', 0):7.3f} ms  p3d "
                      f"{(num(kv, 'p3d', 0) or 0) / upm if upm else 0:7.3f} ms{extra}  "
                      f"{shares(kv)}  {tr_text(kv, 6)}")
        elif prefix == "B2.":
            base = None
            for t in names:
                per = row(t, "us/draw", 1000.0, base)
                if t == "B2.nostate":
                    base = per
        elif prefix == "B3.":
            pts = []
            b3 = {}
            for t in names:
                if t == "B3.cullcheck":
                    kv, rest = tests[t]
                    print(f"  {t:26s} {rest}")
                    continue
                per = row(t, "ns/vertex", 1e6 / 3.0)
                b3[t] = per
                m = re.match(r"B3\.v(\d+)$", t)
                if m and per is not None:
                    pts.append((int(m.group(1)), per))
            if len(pts) >= 2:
                n = len(pts)
                mx = sum(x for x, _ in pts) / n
                my = sum(y for _, y in pts) / n
                sxx = sum((x - mx) ** 2 for x, _ in pts)
                if sxx:
                    slope = sum((x - mx) * (y - my) for x, y in pts) / sxx
                    print(f"  -> {slope:.3f} ns per vertex per shader instruction, "
                          f"{my - slope * mx:.2f} ns per vertex at 0 instructions (fit of B3.v*)")
                    b3["slope"] = slope
            # (AI-assisted) config.ini shader_opt's lines: the opt programs against the new ones
            # (both measured) and against the hardware default (the Oct 2 old programs, 21 / 53
            # instructions: estimated from the fit's slope)
            def ratio(a, b):
                return f"{100.0 * (b3[a] / b3[b] - 1.0):+.1f}%" if b3.get(a) and b3.get(b) else "?"
            if "B3.o3p" in b3:
                o3, o4, o3p = b3.get("B3.o3"), b3.get("B3.o4"), b3.get("B3.o3p")
                if o3 and o4 and o3p is not None:
                    per_reg = abs(o3p - o3) < abs(o3p - o4)
                    print(f"  -> packed output register: o3p {o3p:.2f} vs o3 {o3:.2f} / o4 "
                          f"{o4:.2f} ns/vertex: the setup costs per output "
                          f"{'register (packing texcoord1 helps)' if per_reg else 'semantic (packing changes nothing)'}")
            for opt, new, old_instr, opt_instr in (("B3.mesh_opt", "B3.mesh_new", 21, 10),
                                                  ("B3.skin_opt", "B3.skin_new", 53, 33)):
                if opt in b3:
                    est = ""
                    if b3.get("slope") is not None and b3.get(opt):
                        old_est = b3[opt] + b3["slope"] * (old_instr - opt_instr)
                        est = (f"; the hardware default (old, {old_instr} instructions) is "
                               f"~{old_est:.2f} ns/vertex by the fit: "
                               f"{100.0 * (b3[opt] / old_est - 1.0):+.1f}%")
                    print(f"  -> shader_opt {opt[3:]}: {ratio(opt, new)} against {new[3:]}{est}")
        elif prefix == "B4.":
            for t in names:
                if t == "B4.cachenum":
                    print(f"  {t:26s} {tests[t][1]}")
                    continue
                row(t, "ns/index", 1e6)
        elif prefix == "B5.":
            for t in names:
                row(t, "ns/vertex", 1e6 / 3.0)
        elif prefix == "B6.":
            for t in names:
                kv, rest = tests[t]
                if t.endswith(".probe"):
                    print(f"  {t:26s} bg_ok={kv.get('bg_ok', '?')} {rest}")
                    continue
                if "skipped" in kv:
                    print(f"  {t:26s} skipped ({kv['skipped']})")
                    continue
                ms = gpu_ms(kv)
                n = num(kv, "n", 0) or 0
                mpix = n / (ms * 1000.0) if ms else 0.0
                print(f"  {t:26s} gpu {ms:8.3f} ms  {mpix:7.1f} Mpix/s  "
                      f"{ms * 1e6 / n if n else 0:6.3f} ns/pix  {shares(kv):38s} {tr_text(kv)}")
        elif prefix == "B7.":
            for t in names:
                row(t, "ns/triangle", 1e6)
        elif prefix == "B8.":
            for t in names:
                kv, rest = tests[t]
                flags = []
                for k in ("a", "b", "c"):
                    if f"exp_{k}" in kv and kv.get(k) != kv[f"exp_{k}"]:
                        flags.append(f"{k}={kv.get(k)} (expected {kv[f'exp_{k}']})")
                c = int(kv.get("c", "0") or 0)
                box = kv.get("cbox", "")
                verdict = "OK" if not flags else "DIFF " + "; ".join(flags)
                if c and t.split(".")[-1] not in ("ref",):
                    verdict += f"  clear color box {box}"
                print(f"  {t:26s} a={kv.get('a')} b={kv.get('b')} c={kv.get('c')} "
                      f"other={kv.get('other')} abox={kv.get('abox')}  {verdict}")
    others = [t for t, _, _ in s["tests"] if not re.match(r"B[1-8]\.", t)]
    for t in others:
        print(f"  {t}: {tests[t][1]}")
    if s["end"]:
        e = s["end"]
        print(f"end: {e.get('tests')} tests in {e.get('seconds')} s, stopped={e.get('stopped')}")
    else:
        last = s.get("last_start")
        print("end: MISSING (the bench didn't finish) - " +
              (f"the last test started (bench.txt): {last}" if last else
               "pass bench.txt too: its last `start` line names the test that hung"))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__ or "usage: bench_report.py <gk.log|bench.txt> [--all]", file=sys.stderr)
        return 2
    ss = sessions(args)
    if not ss:
        print("no [bench] lines found", file=sys.stderr)
        return 1
    for s in (ss if "--all" in sys.argv else ss[-1:]):
        report(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
