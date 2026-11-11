#!/usr/bin/env python3
"""(AI-assisted, sched) A rough in-order cycle model of the ARM11 + VFP11 for a loop body in GCC's
assembly output, for comparing compiler options where Azahar (one tick per instruction) can't.

Model: one instruction issued per cycle, in order; an instruction waits until its source
registers are ready. Result latencies (cycles after issue): VFP add / sub / mul / multiply-add 8
(GCC's vfp11 pipeline model, the VFP11 divider paper's one FMAC per cycle), vdiv / vsqrt 19
(and they block the next ones for 15), VFP load 4, integer load 3, integer ALU 1, multiply 3,
vmov between the banks 2, vmrs APSR waits for every VFP result in flight. A load or store
multiple issues over (registers + 1) // 2 cycles. Branches cost 1 (prediction ignored), so this
prices latency and issue only: cache misses and mispredicts are not modelled.

Usage: vfp11_cycles.py FILE.s FUNCTION [LOOP_LABEL] [ITERS]
  the cycles of ITERS (default 8) passes over the loop (from LOOP_LABEL to the branch back to it)
  with registers carried over between passes, divided by ITERS.
"""
import re
import sys

VFP_ARITH = re.compile(r'^v(add|sub|mul|mla|mls|nmla|nmls|nmul|neg|abs|cvt|mov)\.(f32|f64|s32\.f32|f32\.s32|u32\.f32|f32\.u32)')


def regs(s):
    out = []
    for m in re.finditer(r'\b([sdr]\d+|lr|ip|sp|fp)\b', s):
        r = m.group(1)
        r = {'ip': 'r12', 'fp': 'r11', 'sp': 'r13', 'lr': 'r14'}.get(r, r)
        if r.startswith('d'):
            k = int(r[1:])
            out += ['s%d' % (2 * k), 's%d' % (2 * k + 1)]
        else:
            out.append(r)
    return out


def reglist(s):
    m = re.search(r'\{([^}]*)\}', s)
    if not m:
        return []
    out = []
    for part in m.group(1).split(','):
        part = part.strip()
        if '-' in part:
            a, b = [p.strip() for p in part.split('-')]
            kind = a[0]
            for k in range(int(a[1:]), int(b[1:]) + 1):
                out += regs('%s%d' % (kind, k))
        else:
            out += regs(part)
    return out


def body(path, fn, label):
    lines = open(path).read().split('\n')
    start = None
    for i, l in enumerate(lines):
        if l.strip() == fn + ':':
            start = i
            break
    if start is None:
        sys.exit('function %s not found' % fn)
    ins = []
    in_loop = label is None
    for l in lines[start + 1:]:
        t = l.strip()
        if t.startswith('.size'):
            break
        if label and t == label + ':':
            in_loop = True
            continue
        if not t or t.startswith('.') or t.startswith('@') or t.endswith(':'):
            continue
        if in_loop:
            ins.append(t.split('@')[0].strip())
            if label and re.match(r'^b\w*\s+' + re.escape(label) + r'$', t):
                break
    return ins


def simulate(ins, iters):
    ready = {}
    t = 0
    vfp_busy_until = 0
    last_vfp_done = 0
    for _ in range(iters):
        for s in ins:
            op = s.split()[0]
            args = s[len(op):]
            srcs, dsts = [], []
            lat = 1
            issue = 1
            if op.startswith('vldm') or op.startswith('vpop') or op.startswith('ldm') or op == 'pop':
                rl = reglist(args)
                dsts = rl
                srcs = [r for r in regs(args.split('{')[0])]
                lat = 4 if op.startswith('v') else 3
                issue = max(1, (len(rl) + 1) // 2)
            elif op.startswith('vstm') or op.startswith('vpush') or op.startswith('stm') or op == 'push':
                srcs = reglist(args) + regs(args.split('{')[0])
                issue = max(1, (len(reglist(args)) + 1) // 2)
            elif op.startswith('vldr'):
                rs = regs(args)
                dsts, srcs = rs[:2] if rs[0].startswith('s') and len(rs) > 2 and False else rs[:1], rs[1:]
                lat = 4
            elif op.startswith('vstr') or op.startswith('str'):
                srcs = regs(args)
            elif op.startswith('ldr'):
                rs = regs(args)
                dsts, srcs = rs[:1], rs[1:]
                lat = 3
            elif op.startswith('vmrs'):
                # APSR_nzcv: waits for every VFP result in flight
                srcs = []
                t = max(t, last_vfp_done)
                dsts = ['cpsr'] if 'APSR' in args else regs(args)[:1]
            elif op.startswith('vcmp'):
                srcs = regs(args)
                dsts = []
                lat = 8
                last_vfp_done = max(last_vfp_done, t + 1 + 4)
            elif op.startswith('vdiv') or op.startswith('vsqrt'):
                rs = regs(args)
                dsts, srcs = rs[:1], rs[1:]
                lat = 19
                t = max(t, vfp_busy_until)
                vfp_busy_until = t + 15
            elif VFP_ARITH.match(op) or op.startswith('vmov'):
                rs = regs(args)
                dsts, srcs = rs[:1], rs[1:]
                if op.startswith('vmov') and (any(r.startswith('r') for r in rs)):
                    lat = 2
                elif op.startswith('vmov') and op == 'vmov.f32':
                    lat = 4
                else:
                    lat = 8
                if op.startswith(('vmla', 'vmls', 'vnmla', 'vnmls')):
                    srcs = srcs + dsts  # the accumulator
            elif op.startswith('b') or op.startswith('cmp') or op.startswith('tst') or op.startswith('cmn'):
                srcs = regs(args)
            elif op.startswith('mul') or op.startswith('mla') or op.startswith('smul') or op.startswith('umul'):
                rs = regs(args)
                dsts, srcs = rs[:1], rs[1:]
                lat = 3
            else:
                rs = regs(args)
                dsts, srcs = rs[:1], rs[1:]
                if '!' in args:
                    dsts = rs[:1]
            start = t
            for r in srcs:
                start = max(start, ready.get(r, 0))
            for r in dsts:
                ready[r] = start + lat
                if r.startswith('s'):
                    last_vfp_done = max(last_vfp_done, start + lat)
            t = start + issue
    return t / iters


if __name__ == '__main__':
    path, fn = sys.argv[1], sys.argv[2]
    label = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != '-' else None
    iters = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    ins = body(path, fn, label)
    print('%s %s: %d instructions, %.1f cycles per pass' % (path, fn, len(ins), simulate(ins, iters)))
