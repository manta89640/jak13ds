#!/usr/bin/env python3
# (AI-assisted) Refuse vertex shaders with output-write patterns never proven on a real 3DS.
# 2026-10-03: the new ctr_mesh.v.pica wrote texcoord1 in parts (x from a max with a negated
# source, y separately, zw never) and hung the real GPU; Azahar ran it fine. Partial writes as
# such are fine on hardware (the Oct 2 ctr_skin writes its color as .w then .xyz). Rule: every
# component of every output is written at least once.
#   check_shaders.py <dir with .v.pica files>
import re, sys, pathlib
bad = 0
for f in sorted(pathlib.Path(sys.argv[1]).glob("*.v.pica")):
    outs = {}
    for line in f.read_text().splitlines():
        m = re.match(r"\s*\.out\s+(\w+)\s+(\w+)", line)
        if m:
            outs[m.group(1)] = m.group(2)
    writes = {o: [] for o in outs}
    for n, line in enumerate(f.read_text().splitlines(), 1):
        code = line.split(";")[0].strip()
        m = re.match(r"[a-z0-9]+\s+(\w+)(\.[xyzw]+)?\s*,", code)
        if m and m.group(1) in outs:
            writes[m.group(1)].append((n, m.group(2) or ""))
    for o, w in writes.items():
        comps = set()
        for _, m in w:
            comps |= set(m[1:] if m else "xyzw")
        if comps != set("xyzw"):
            print(f"{f.name}: output {o} ({outs[o]}): components {''.join(sorted(set('xyzw') - comps))} "
                  f"never written (writes at lines {[n for n, _ in w]})")
            bad += 1
sys.exit(1 if bad else 0)
