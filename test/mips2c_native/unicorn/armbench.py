#!/usr/bin/env python3
# (AI-assisted)
# Runs armbench.elf (armbench.cpp) under Unicorn on every case file the harness wrote with --dump:
# the mips2c version, then the native one from the same memory, compares v0 and GOAL memory
# (except the stack area), and prints the ARM instructions per call of each (their own code, and
# with the 3DS's memcpy / memmove / memset, ctr_mem.S, which the copies of both versions call).
#
#   armbench.py <armbench.elf> <armbench.map> <case dir> <function> [--no-v0]
#
# (AI-assisted) --no-v0: v0 isn't compared (natives of GOAL functions that return a flag the GOAL
# function doesn't)

import glob
import os
import re
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK, UcError
from unicorn.arm_const import (UC_CPU_ARM_1176, UC_ARM_REG_SP, UC_ARM_REG_FPEXC,
                               UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPSCR, UC_ARM_REG_PC)

PROG = 0x00100000
PROG_SIZE = 8 << 20
STACK = 0x0E000000
STACK_SIZE = 1 << 20
JOB = 0x0F000000
JOB_SIZE = 0x20000
GOAL = 0x10000000


def load_elf(path):
    """[(vaddr, bytes)] of the PT_LOAD segments, the entry, and {symbol: (addr, size)}"""
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 1  # 32-bit
    (e_entry, e_phoff, e_shoff) = struct.unpack_from("<III", data, 24)
    (e_phentsize, e_phnum, e_shentsize, e_shnum, _) = struct.unpack_from("<HHHHH", data, 42)
    segs = []
    for i in range(e_phnum):
        (p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, _, _) = struct.unpack_from(
            "<IIIIIIII", data, e_phoff + i * e_phentsize)
        if p_type == 1:
            body = data[p_offset:p_offset + p_filesz]
            segs.append((p_vaddr, body + b"\0" * (p_memsz - p_filesz)))
    syms = {}
    sections = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize)
                for i in range(e_shnum)]
    for sh in sections:
        if sh[1] == 2:  # SHT_SYMTAB
            strtab = sections[sh[6]]
            for k in range(sh[5] // 16):
                (st_name, st_value, st_size, st_info, _, _) = struct.unpack_from(
                    "<IIIBBH", data, sh[4] + k * 16)
                end = data.index(b"\0", strtab[4] + st_name)
                syms[data[strtab[4] + st_name:end].decode()] = (st_value & ~1, st_size)
    return segs, e_entry, syms


def code_ranges(map_file, prefixes):
    """merged [(start, end)] of the .text* input sections of the objects whose name starts with one
    of prefixes (as bench.py does with qemu's map)"""
    ranges = []
    lines = open(map_file).read().splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"\s*(\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if not m and re.match(r"\s*\.text\S*\s*$", line) and i + 1 < len(lines):
            m = re.match(r"\s*()0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", lines[i + 1])
        if not m:
            continue
        base = m.group(4).split("/")[-1]
        if not any(base.startswith(p) for p in prefixes):
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


def read_case(path):
    data = open(path, "rb").read()
    assert data[:4] == b"M2CD"
    (version, mem_size, pp, st, stack_bottom, stack_top, nsyms) = struct.unpack_from("<7I", data, 4)
    args = struct.unpack_from("<8Q", data, 32)
    off = 96
    syms = []
    for _ in range(nsyms):
        addr, n = struct.unpack_from("<II", data, off)
        off += 8
        syms.append((data[off:off + n].decode(), addr))
        off += n
    mem = data[off:off + mem_size]
    return dict(pp=pp, st=st, stack_bottom=stack_bottom, stack_top=stack_top, args=args, syms=syms,
                mem=mem)


def in_ranges(ranges, a):
    lo, hi = 0, len(ranges)
    while lo < hi:
        mid = (lo + hi) // 2
        if ranges[mid][1] <= a:
            lo = mid + 1
        else:
            hi = mid
    return lo < len(ranges) and ranges[lo][0] <= a < ranges[lo][1]


def run(segs, entry, done, case, function, version, own, libc):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    uc.mem_map(PROG, PROG_SIZE)
    uc.mem_map(STACK, STACK_SIZE)
    uc.mem_map(JOB, JOB_SIZE)
    mem_size = (len(case["mem"]) + 0xFFF) & ~0xFFF
    uc.mem_map(GOAL, mem_size)
    for vaddr, b in segs:
        uc.mem_write(vaddr, b)
    uc.mem_write(GOAL, case["mem"])
    job = struct.pack("<II8QQQII", version, len(case["syms"]), *case["args"], case["pp"],
                      case["st"], case["stack_top"], 0)
    job += struct.pack("<Q", 0) + function.encode().ljust(64, b"\0")
    for name, addr in case["syms"]:
        job += struct.pack("<I", addr) + name.encode()[:59].ljust(60, b"\0")
    assert len(case["syms"]) <= 1500 and len(job) <= JOB_SIZE
    uc.mem_write(JOB, job)
    # VFP on (CPACR cp10/cp11, FPEXC.EN), the 3DS's mode: default NaN, flush to zero
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(UC_ARM_REG_FPSCR, 0x03000000)
    uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SIZE)
    counts = [0, 0]

    def block(uc_, address, size, user):
        if in_ranges(own, address):
            counts[0] += size // 4
        elif in_ranges(libc, address):
            counts[1] += size // 4

    uc.hook_add(UC_HOOK_BLOCK, block)
    try:
        uc.emu_start(entry, done, count=2_000_000_000)
    except UcError as e:
        raise RuntimeError(f"unicorn: {e} at pc 0x{uc.reg_read(UC_ARM_REG_PC):x}")
    status, = struct.unpack("<I", uc.mem_read(JOB + 92, 4))
    v0, = struct.unpack("<Q", uc.mem_read(JOB + 96, 8))
    mem = bytes(uc.mem_read(GOAL, len(case["mem"])))
    return status, v0, mem, counts


def main():
    elf, map_file, case_dir, function = sys.argv[1:5]
    check_v0 = "--no-v0" not in sys.argv[5:]
    segs, entry, syms = load_elf(elf)
    done = syms["armbench_done"][0]
    m2c = code_ranges(map_file, ["m2c_"])
    nat = code_ranges(map_file, ["native_"])
    libc = code_ranges(map_file, ["ctr_mem"])  # the 3DS's memcpy / memmove / memset
    files = sorted(glob.glob(os.path.join(case_dir, "*.m2cd")),
                   key=lambda p: int(p.rsplit(".", 2)[-2]))
    ok = bad = skipped = 0
    tot = {"mips2c": [0, 0], "native": [0, 0]}
    for path in files:
        case = read_case(path)
        sm, vm, mm, cm = run(segs, entry, done, case, function, 0, m2c, libc)
        if sm != 1:
            skipped += 1  # the mips2c version can't run it (an ASSERT): not a valid case
            continue
        sn, vn, mn, cn = run(segs, entry, done, case, function, 1, nat, libc)
        lo, hi = case["stack_bottom"], case["stack_top"]
        same = sn == 1 and (vm == vn or not check_v0) and mm[:lo] == mn[:lo] and mm[hi:] == mn[hi:]
        if same:
            ok += 1
        else:
            bad += 1
            if bad <= 3:
                what = (f"status {sn}" if sn != 1 else
                        (f"v0 {vm:x} {vn:x}" if vm != vn and check_v0 else "memory"))
                print(f"  MISMATCH {os.path.basename(path)}: {what}")
                if sn == 1 and (vm == vn or not check_v0):
                    for a in range(0, len(mm), 16):
                        if lo <= a < hi:
                            continue
                        if mm[a:a + 16] != mn[a:a + 16]:
                            print(f"    0x{a:06x} mips2c {mm[a:a + 16].hex()} "
                                  f"native {mn[a:a + 16].hex()}")
                            break
        for k in range(2):
            tot["mips2c"][k] += cm[k]
            tot["native"][k] += cn[k]
    n = max(ok + bad, 1)
    print(f"{function}: {ok + bad} cases ({skipped} invalid), {bad} mismatches (ARM, 3DS VFP mode)")
    print(f"{'ARM instructions per call':30s} {'mips2c':>10s} {'native':>10s} {'ratio':>7s}")
    for label, k in (("own code", 0), ("with memcpy/memset", None)):
        m = tot["mips2c"][0] + (tot["mips2c"][1] if k is None else 0)
        v = tot["native"][0] + (tot["native"][1] if k is None else 0)
        print(f"{label:30s} {m / n:10.0f} {v / n:10.0f} {m / max(v, 1):6.1f}x")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
