#!/usr/bin/env python3
# (AI-assisted) Per vertex program in assembled .shbin files: instructions run per vertex (main to
# endmain, including `end`; the shaders have no flow control), output registers (the outmap total
# the GPU is set up with), output semantics, output clock bits (GPUREG_SH_OUTATTR_CLOCK as libctru's
# DVLE_GenerateOutmap computes it), constants (uploaded at every program switch), MOVA count.
#   shader_stats.py build-3ds/*.shbin
import pathlib
import struct
import sys

SEM = {0: "pos", 1: "nquat", 2: "clr", 3: "tc0", 4: "tc0w", 5: "tc1", 6: "tc2", 8: "view"}
CLOCK = {1: 1 << 24, 2: 1 << 1, 3: 1 << 8, 4: 1 << 16, 5: 1 << 9, 6: 1 << 10, 8: 1 << 24}


def stats(path):
    b = pathlib.Path(path).read_bytes()
    w = struct.unpack("<%dI" % (len(b) // 4), b[:len(b) // 4 * 4])
    ndvle = w[1]
    dvlp = 2 + ndvle
    code = w[dvlp + w[dvlp + 2] // 4: dvlp + w[dvlp + 2] // 4 + w[dvlp + 3]]
    rows = []
    for i in range(ndvle):
        base = w[2 + i] // 4
        d = w[base:]
        main, endmain = d[2], d[3]
        nconst = d[7]
        outoff, nout = d[10], d[11]
        regs, sems, clock = set(), [], 0
        for k in range(nout):
            o = base * 4 + outoff + 8 * k
            typ, reg, mask = struct.unpack("<HHB", b[o:o + 5])
            regs.add(reg)
            sems.append("%s:o%d.%s" % (SEM.get(typ, "?%d" % typ), reg,
                                       "".join(c for j, c in enumerate("xyzw") if mask >> j & 1)))
            clock |= CLOCK.get(typ, 0)
        movas = sum(1 for x in code[main:endmain] if (x >> 26) == 0x12)
        sym = ""
        rows.append((pathlib.Path(path).stem, i, endmain - main, len(regs), clock, nconst, movas,
                     " ".join(sems)))
    return rows


def main():
    print(f"{'shbin':24s} {'dvle':>4s} {'instr':>5s} {'oregs':>5s} {'clock':>10s} {'consts':>6s} "
          f"{'mova':>4s}  outputs")
    for f in sys.argv[1:]:
        for name, i, n, regs, clock, nconst, movas, sems in stats(f):
            print(f"{name:24s} {i:4d} {n:5d} {regs:5d} {clock:#10x} {nconst:6d} {movas:4d}  {sems}")


if __name__ == "__main__":
    main()
