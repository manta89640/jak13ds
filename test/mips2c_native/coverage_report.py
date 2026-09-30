#!/usr/bin/env python3
# (AI-assisted)
# Line coverage of the mips2c execute functions reached by the differential tests
# (run.sh coverage). Lists the lines of each function that no case executed.
#
#   coverage_report.py <coverage build dir> <mips2c source dir> [--lines]

import os
import re
import subprocess
import sys


def gcov_lines(obj_dir, src):
    """{line number: count or None (not code)} for src, from the m2c_<name>.gcda next to it."""
    name = os.path.splitext(os.path.basename(src))[0]
    obj = os.path.join(obj_dir, "m2c_" + name + ".o")
    if not os.path.exists(os.path.join(obj_dir, "m2c_" + name + ".gcda")):
        return None
    out = subprocess.run(["gcov", "-t", "-o", obj, src], capture_output=True, text=True).stdout
    lines = {}
    current = None
    for l in out.splitlines():
        m = re.match(r"\s*([^:]+):\s*(\d+):(.*)", l)
        if not m:
            continue
        count, num = m.group(1).strip(), int(m.group(2))
        if num == 0:
            if m.group(3).startswith("Source:"):
                current = m.group(3)[len("Source:"):]
            continue
        if current is None or os.path.basename(current) != os.path.basename(src):
            continue  # an included header
        if count == "-":
            lines[num] = None
        elif count.startswith("#") or count.startswith("="):
            lines[num] = 0
        else:
            lines[num] = int(count.rstrip("*"))
    return lines


def functions(src):
    """[(name, first line, last line)] of the execute functions (1-based, inclusive)"""
    text = open(src).read().splitlines()
    result = []
    ns = None
    start = None
    for i, l in enumerate(text, 1):
        m = re.match(r"\s*namespace (\w+) \{", l)
        if m and m.group(1) not in ("Mips2C", "jak1"):
            ns = m.group(1)
        if re.match(r"\s*u64 execute\(void\* ctxt\) \{", l):
            start = i
        if start and re.match(r"\s*end_of_function:", l):
            result.append((ns, start, i))
            start = None
    return result


def main():
    obj_dir, src_dir = sys.argv[1], sys.argv[2]
    show_lines = "--lines" in sys.argv
    total_missed = 0
    for f in sorted(os.listdir(src_dir)):
        if not f.endswith(".cpp") or f.startswith("native_"):
            continue
        src = os.path.join(src_dir, f)
        cov = gcov_lines(obj_dir, src)
        if cov is None:
            continue
        text = open(src).read().splitlines()
        for name, a, b in functions(src):
            code = [n for n in range(a, b + 1) if cov.get(n) is not None]
            if not code or all(cov[n] == 0 for n in code):
                continue  # not tested
            missed = [n for n in code if cov[n] == 0]
            # lines after a goto/return that can't run (dead code the converter left) don't count
            dead = [n for n in missed if "sll r0, r0, 0" in text[n - 1] or "daddu sp, sp, r0" in text[n - 1]]
            missed = [n for n in missed if n not in dead]
            pct = 100.0 * (len(code) - len(missed)) / len(code)
            print(f"{f}:{name:45s} {pct:6.1f}% of {len(code)} lines, {len(missed)} not run")
            total_missed += len(missed)
            if show_lines:
                for n in missed:
                    print(f"    {n}: {text[n - 1].strip()}")
    print(f"lines not run in tested functions: {total_missed}")


if __name__ == "__main__":
    main()
