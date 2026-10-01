#!/usr/bin/env python3
# (AI-assisted)
# ARM instructions per call of the mips2c and the native version of tested functions, counted
# with qemu-arm on the arm build (run.sh bench). Only instructions in the code of the mips2c,
# native and goalc reference object files count (not the harness, the fake GOAL functions or
# libc), so functions they call directly or through mips2c are included, and so are the GOAL
# functions compiled to C (goalc_ref/) the mips2c versions call where the natives have their own;
# fake GOAL callees are not.
#
#   bench.py <arm binary> <link map> [--scale X] test-name...

import re
import subprocess
import sys


def code_ranges(map_file):
    """[(start, end)] of every .text* input section of the native_*.o, m2c_*.o and ref_*.o
    objects (goalc_context.o is a ref_ object too, but only switches stacks)"""
    ranges = []
    lines = open(map_file).read().splitlines()
    for i, line in enumerate(lines):
        # "<section> <addr> <size> <object>", or the section name alone with the rest on the next line
        m = re.match(r"\s*(\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if not m and re.match(r"\s*\.text\S*\s*$", line) and i + 1 < len(lines):
            m = re.match(r"\s*()0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", lines[i + 1])
        if not m:
            continue
        base = m.group(4).split("/")[-1]
        if not (base.startswith("native_") or base.startswith("m2c_") or
                (base.startswith("ref_") and base != "ref_goalc_context.o")):
            continue
        start, size = int(m.group(2), 16), int(m.group(3), 16)
        if size:
            ranges.append((start, start + size))
    ranges.sort()
    merged = []
    for a, b in ranges:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    return merged


def count(binary, rng, test, version, scale):
    dfilter = ",".join(f"0x{a:x}..0x{b - 1:x}" for a, b in rng)
    cmd = ["qemu-arm", "-one-insn-per-tb", "-d", "exec,nochain", "-dfilter", dfilter,
           "-D", "/dev/stderr", binary, "--bench", version,
           "--filter", test, "--scale", str(scale)]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    n = 0
    for line in p.stderr:
        if line.startswith("Trace"):
            n += 1
    out = p.stdout.read()
    p.wait()
    m = re.search(r"(\d+) cases run", out)
    cases = int(m.group(1)) if m else 0
    return n, cases


def main():
    binary, map_file = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    scale = 0.05
    if args and args[0] == "--scale":
        scale = float(args[1])
        args = args[2:]
    rng = code_ranges(map_file)
    print(f"{len(rng)} code ranges, {sum(b - a for a, b in rng)} bytes")
    print(f"{'function':45s} {'mips2c':>10s} {'native':>10s} {'ratio':>7s}   (ARM instructions per call)")
    for test in args:
        m_n, m_cases = count(binary, rng, test, "mips2c", scale)
        n_n, n_cases = count(binary, rng, test, "native", scale)
        if not m_cases or not n_cases:
            print(f"{test:45s} no cases")
            continue
        m_per, n_per = m_n / m_cases, n_n / n_cases
        print(f"{test:45s} {m_per:10.0f} {n_per:10.0f} {m_per / max(n_per, 1):6.1f}x")


if __name__ == "__main__":
    main()
