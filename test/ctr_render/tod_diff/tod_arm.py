#!/usr/bin/env python3
# (AI-assisted) tod_diff on ARM: tod_arm.elf under Unicorn (ARM1176), one version's ARM instructions
# per level draw: tod_update, the group pass, the light pass (all of the code run between the marks).
#   tod_arm.py <tod_arm.elf> <level.tod> <trace> <frames> ref|neu <stride> <pulse_step> [skip]
#              [lines]
# skip: the first level draws left out of the means (the lists being made); lines: also the
# chunk meshes' 32-byte cache lines each level draw reads and writes (every load and store into
# the version's mesh block; PLD is no access) and the bytes its cache flushes cover. FCRAM model:
# every line touched is filled once, every line written is written back once.
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'mips2c_native', 'unicorn'))
from armbench import load_elf  # noqa: E402
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE,  # noqa: E402
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE)
from unicorn.arm_const import (UC_CPU_ARM_1176, UC_ARM_REG_SP, UC_ARM_REG_R0,  # noqa: E402
                               UC_ARM_REG_FPEXC, UC_ARM_REG_C1_C0_2)

elf, level, trace, frames, who, stride, step = sys.argv[1:8]
skip = int(sys.argv[8]) if len(sys.argv) > 8 else 0
lines = len(sys.argv) > 9 and sys.argv[9] == 'lines'
extra_mode = int(os.environ.get('TOD_ARM_MODE', '0'))  # tod_diff.cpp's mode bits (128: title)

segs, entry, syms = load_elf(elf)
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
uc.mem_map(0x00100000, 96 << 20)  # program and heap
uc.mem_map(0x0c000000, 16 << 20)  # trace
uc.mem_map(0x0e000000, 1 << 20)  # stack
uc.mem_map(0x0f000000, 1 << 16)  # job
d = open(level, 'rb').read()
uc.mem_map(0x20000000, (len(d) + 0xffff) & ~0xffff)
uc.mem_write(0x20000000, d)
vals = [int(x) for x in open(trace).read().split()]
uc.mem_write(0x0c000000, struct.pack('<%dI' % len(vals), *vals))
mode = ((1 | 8) if who == 'ref' else (2 | 16)) | extra_mode
uc.mem_write(0x0f000000, struct.pack('<4I', int(frames), mode, int(stride), int(step)))
for vaddr, b in segs:
    uc.mem_write(vaddr, b)
uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
uc.reg_write(UC_ARM_REG_SP, 0x0e100000)
count = [0]
cur = [0, 0, 0]
draws = []


def block(uc_, addr, size, user):
    count[0] += size // 4


touched = set()
written = set()
line_draws = []
flush_at = syms['g_tod_flush_bytes'][0]


def access(uc_, kind, addr, size, value, user):
    ln = addr >> 5
    touched.add(ln)
    if kind == UC_MEM_WRITE_KIND:
        written.add(ln)


def mark(uc_, addr, size, user):
    ph = uc_.reg_read(UC_ARM_REG_R0)
    if 1 <= ph <= 3:
        cur[ph - 1] = count[0]
    count[0] = 0
    if ph == 0:
        touched.clear()
        written.clear()
        uc_.mem_write(flush_at, b"\0\0\0\0")
    if ph == 3:
        draws.append(tuple(cur))
        fl = struct.unpack('<I', uc_.mem_read(flush_at, 4))[0]
        line_draws.append((len(touched), len(written), fl))


uc.hook_add(UC_HOOK_BLOCK, block)
if lines:
    from unicorn import UC_MEM_WRITE as UC_MEM_WRITE_KIND  # noqa: E402
    a0, n0 = syms['g_arena_ref' if who == 'ref' else 'g_arena_neu']
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access, begin=a0, end=a0 + n0 - 1)
m = syms['sim_mark'][0]
uc.hook_add(UC_HOOK_CODE, mark, begin=m, end=m)
uc.emu_start(entry, syms['tod_arm_done'][0], count=40_000_000_000)
draws = draws[skip:]
n = max(len(draws), 1)
for k, name in enumerate(['tod_update', 'group pass', 'light pass']):
    xs = [x[k] for x in draws]
    print(f'  {name:11s} mean {sum(xs) / n:9.0f}  most {max(xs):9d}')
tot = [sum(x) for x in draws]
print(f'  {"total":11s} mean {sum(tot) / n:9.0f}  most {max(tot):9d}  ({len(draws)} level draws)')
if lines:
    ld = line_draws[skip:]
    for k, name in enumerate(['lines touched', 'lines written', 'bytes flushed']):
        xs = [x[k] for x in ld]
        print(f'  {name:13s} mean {sum(xs) / n:9.0f}  most {max(xs):9d}')
    traffic = [x[0] + x[1] for x in ld]
    print(f'  {"line moves":13s} mean {sum(traffic) / n:9.0f}  most {max(traffic):9d}'
          '  (fills + write-backs)')
