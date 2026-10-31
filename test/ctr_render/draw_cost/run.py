#!/usr/bin/env python3
# (AI-assisted) run.py <elf> <mode> <count> <seed> <states> [words.bin]: harness.c under Unicorn,
# the ARM instructions per call by state path (same / texture only / whole), the command words (and
# the final combiner and fragment state) written to words.bin. SORTED=1: mode 2 a chunk's draws together.
import struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'mips2c_native', 'unicorn'))
from armbench import load_elf
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.arm_const import *
elf, mode, count, seed, nst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
segs, entry, syms = load_elf(elf)
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
end = max(v + len(b) for v, b in segs)
uc.mem_map(0x00100000, ((end - 0x00100000) + (8 << 20) + 0xfffff) & ~0xfffff)
uc.mem_map(0x30000000, 4 << 20); uc.mem_map(0x0e000000, 1 << 20)
uc.mem_map(0x0f000000, 1 << 16)
uc.mem_map(0x1f000000, 1 << 20)  # (svc stubs)
for v, b in segs:
    uc.mem_write(v, b)
uc.mem_write(0x0f000000, struct.pack('<5I', mode, count, seed, nst, int(os.environ.get('SORTED', '0'))))
uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
cnt = [0]
def block(u, a, s, d):
    cnt[0] += s // 4
uc.hook_add(UC_HOOK_BLOCK, block)
def intr(u, n, d):  # svc: return 0
    u.reg_write(UC_ARM_REG_R0, 0)
    u.reg_write(UC_ARM_REG_R1, 0)
uc.hook_add(UC_HOOK_INTR, intr)
st = {'t0': 0, 'last': 0}
by = {0: [], 1: [], 2: []}
def mark(u, a, s, d):
    k = u.reg_read(UC_ARM_REG_R0)
    if k == 1:
        st['t0'] = cnt[0]
    elif k == 2:
        st['last'] = cnt[0] - st['t0']
    elif k >= 10:
        by[k - 10].append(st['last'])
m = syms['h_mark'][0]
uc.hook_add(UC_HOOK_CODE, mark, begin=m, end=m)
uc.emu_start(entry, syms['h_done'][0], count=2_000_000_000)
names = ['same', 'fast', 'full']
tot = sum(sum(v) for v in by.values()); n = sum(len(v) for v in by.values())
print(f'{os.path.basename(elf)} mode {mode}: {n} calls, {tot / max(n,1):.0f} instructions each; ' +
      ', '.join(f'{names[k]} {len(v)} x {sum(v) / max(len(v),1):.0f}' for k, v in by.items()))
job = uc.mem_read(0x0f000000, 0x200)
off, buf = struct.unpack_from('<2I', job, 32)
words = uc.mem_read(buf, off * 4)
if len(sys.argv) > 6:
    open(sys.argv[6], 'wb').write(bytes(words) + bytes(job[64:64 + 4 * 80]))
