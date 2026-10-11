#!/usr/bin/env python3
# (AI-assisted) Refuse vertex shaders with output-write patterns that can freeze a real 3DS GPU.
# GBATEK "3DS GPU Pitfalls": "Each configured output component has to be written exactly once or
# the PICA freezes", and "Having 2 consecutive MOVA instructions will freeze the PICA". Azahar
# runs such shaders fine; the hardware hangs (2026-10-03: the new ctr_mesh.v.pica wrote
# texcoord1 in parts, zw never, and hung the real GPU). Rules, per .v.pica file:
#  - every component of every .out is written exactly once (partial writes are fine as long as
#    the components add up: the Oct 2 ctr_skin writes its color as .w then .xyz)
#  - outputs are written through their .out names only (a raw oN write can't be checked)
#  - no flow control (if/loop/call/jmp/break...): writes on several paths can't be counted
#  - no two MOVA instructions in a row
#   check_shaders.py <dir with .v.pica files>
import re
import sys
import pathlib

FLOW = re.compile(r"(ifu|ifc|if|loop|callu|callc|call|jmpu|jmpc|jmp|breakc|break|else|endif|"
                  r"endloop)\b")
WRITE = re.compile(r"([a-z0-9]+)\s+(\w+)(\.[xyzw]+)?(\[[^\]]*\])?\s*,")


def check(f):
    errors = []
    lines = f.read_text().splitlines()
    outs = {}
    for line in lines:
        m = re.match(r"\s*\.out\s+(\w+)\s+(\w+)", line)
        if m:
            outs[m.group(1)] = m.group(2)
    count = {(o, c): [] for o in outs for c in "xyzw"}
    prev_mova = None
    for n, line in enumerate(lines, 1):
        code = line.split(";")[0].strip()
        if not code or code.startswith(".") or code.endswith(":"):
            continue
        if FLOW.match(code):
            errors.append(f"line {n}: flow control ({code.split()[0]}): output writes can't be "
                          f"counted per path")
        op = code.split()[0]
        if op == "mova":
            if prev_mova is not None:
                errors.append(f"lines {prev_mova}-{n}: two MOVA instructions in a row")
            prev_mova = n
        else:
            prev_mova = None
        m = WRITE.match(code)
        if not m:
            continue
        dst = m.group(2)
        if re.fullmatch(r"o\d+", dst):
            errors.append(f"line {n}: writes {dst} directly (write outputs through their .out "
                          f"names)")
            continue
        if dst in outs:
            for c in (m.group(3)[1:] if m.group(3) else "xyzw"):
                count[(dst, c)].append(n)
    for o in outs:
        never = "".join(c for c in "xyzw" if not count[(o, c)])
        if never:
            errors.append(f"output {o} ({outs[o]}): components {never} never written")
        for c in "xyzw":
            if len(count[(o, c)]) > 1:
                errors.append(f"output {o} ({outs[o]}): .{c} written {len(count[(o, c)])} times "
                              f"(lines {count[(o, c)]})")
    return errors


def main():
    bad = 0
    files = sorted(pathlib.Path(sys.argv[1]).glob("*.v.pica"))
    for f in files:
        for e in check(f):
            print(f"{f.name}: {e}")
            bad += 1
    if not bad:
        print(f"check_shaders: {len(files)} shaders, every output component written exactly once")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
