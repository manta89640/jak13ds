#!/usr/bin/env python3
# (AI-assisted) Decodes the GPU command lists written by config.ini cmd_dump (data/log/cmd_NN.bin,
# ctr_gpu_citro3d.c cmd_dump_frame) and reports where the command words go:
#  - words per frame by register group (draw template, uniforms, shader code, texture units, ...)
#  - draws per kind (from the vertex layout of attribute buffer 0 and the uploaded vertex program)
#    and the words spent between one draw and the next, per kind
#  - register writes that change nothing (the same value the register already has, under the
#    byte mask) and float uniform vectors written with the value they already hold: what a
#    dedup could drop. The first frame's state is unknown: a register counts only from its
#    second write on.
# Command format (3dbrew GPU/Commands): parameter word, header (register id bits 0-15, byte mask
# 16-19, extra parameters 20-27, consecutive registers bit 31), the extra parameters, padding to
# 8 bytes.
#   cmd_report.py [--shbin-dir build-3ds] cmd_00.bin [cmd_01.bin ...]
#   cmd_report.py --kicks A.bin B.bin   (AI-assisted, micro-gpu: the GPU state at every draw of two
#   dumps compared; config.ini gpu_micro_ident makes off / on pairs of one paused view)
import argparse
import collections
import difflib
import pathlib
import re
import struct
import sys

REGS_H = pathlib.Path.home() / "devkitpro-3ds/src/libctru/libctru/include/3ds/gpu/registers.h"


def reg_names():
    names = {}
    try:
        for m in re.finditer(r"#define GPUREG_(\w+)\s+0x([0-9A-Fa-f]+)", REGS_H.read_text()):
            names.setdefault(int(m.group(2), 16), m.group(1))
    except OSError:
        pass
    return names


NAMES = reg_names()
# data ports: every write is new data (uniforms, shader code, LUTs, fixed attributes)
PORTS = set(range(0x2C1, 0x2C9)) | set(range(0x2CC, 0x2D4)) | set(range(0x2D6, 0x2DE)) | \
    set(range(0x291, 0x299)) | set(range(0x29C, 0x2A4)) | set(range(0x2A6, 0x2AE)) | \
    set(range(0x1C8, 0x1D0)) | set(range(0x0E8, 0x0F0)) | set(range(0x0B0, 0x0B8)) | \
    set(range(0x233, 0x236))
# writes that do something (draw, clear a cache, start a transfer): never "redundant"
TRIGGERS = {0x010, 0x110, 0x111, 0x22E, 0x22F, 0x231, 0x25F, 0x232, 0x2C0, 0x290, 0x2CB, 0x29B,
            0x2D5, 0x2A5, 0x1C5, 0x0E6, 0x0AE, 0x245, 0x23A, 0x2BF, 0x28F}
TEXCLEAR = 0x080  # TEXUNIT_CONFIG: bit 16 clears the texture cache


def group(reg):
    if reg in (0x2C0,) or 0x2C1 <= reg <= 0x2C8:
        return "vs float uniforms"
    if 0x2B0 <= reg <= 0x2BF or reg in (0x2CA, 0x2CB, 0x2D5) or 0x2CC <= reg <= 0x2DD:
        return "vs config+code"
    if 0x280 <= reg <= 0x2AF:
        return "gs config"
    if 0x200 <= reg <= 0x227 and reg not in (0x227,):
        return "attribute buffers"
    if reg in (0x227, 0x228, 0x229, 0x22A, 0x22E, 0x22F, 0x231, 0x245, 0x25E, 0x25F, 0x253,
               0x229, 0x232, 0x233, 0x234, 0x235, 0x242, 0x24A, 0x251, 0x252, 0x254):
        return "draw/primitive"
    if 0x050 <= reg <= 0x06F or reg in (0x04D, 0x04E, 0x04F, 0x041, 0x042, 0x043, 0x044, 0x045,
                                        0x046, 0x047, 0x048, 0x049, 0x04A, 0x04B, 0x04C, 0x040,
                                        0x068, 0x06D, 0x06E):
        return "rasterizer/outmap"
    if 0x080 <= reg <= 0x0AF:
        return "texture units"
    if 0x0C0 <= reg <= 0x0FF:
        return "texture combiners/fog"
    if 0x100 <= reg <= 0x13F:
        return "framebuffer/fragment ops"
    if 0x140 <= reg <= 0x1DF:
        return "fragment lighting"
    return "other"


class Shbins:
    """vertex programs by the first code words uploaded (+ entry point), from the .shbin files"""

    def __init__(self, d):
        self.known = []
        if not d:
            return
        for f in sorted(pathlib.Path(d).glob("*.shbin")):
            b = f.read_bytes()
            w = struct.unpack("<%dI" % (len(b) // 4), b[:len(b) // 4 * 4])
            ndvle = w[1]
            dvlp = 2 + ndvle
            code_off, code_n = w[dvlp + 2] // 4, w[dvlp + 3]
            code = tuple(w[dvlp + code_off: dvlp + code_off + code_n])
            mains = [w[w[2 + i] // 4 + 2] for i in range(ndvle)]
            self.known.append((code, mains, f.stem))

    def name(self, code, entry):
        for c, mains, stem in self.known:
            if code[:len(c)] == c and len(code) - len(c) < 8:
                if len(mains) == 1:
                    return stem
                if entry in mains:
                    return "%s#%d" % (stem, mains.index(entry))
                return stem
        return "prog?"


def decode(words, shbins):
    regs = {}
    seen = set()
    unif = {}
    st = dict(words=collections.Counter(), redundant=collections.Counter(),
              redundant_words=collections.Counter(), unif_vecs=0, unif_redundant=0,
              unif_red_words=0.0, draws=collections.Counter(), draw_words=collections.Counter(),
              total=len(words), texclears=0, prog_switches=0,
              unif_red_by=collections.Counter(), draw_verts=collections.Counter())
    unif_idx, unif_f32, unif_buf = 0, False, []
    code_buf, code_cur, entry = [], (), 0
    since_draw = 0
    i = 0
    n = len(words)
    while i + 1 < n:
        param, hdr = words[i], words[i + 1]
        reg, mask, extra, seq = hdr & 0xFFFF, (hdr >> 16) & 0xF, (hdr >> 20) & 0xFF, hdr >> 31
        vals = [param] + list(words[i + 2: i + 2 + extra])
        size = 2 + extra + ((extra & 1))  # header pair + extras, padded to 8 bytes
        i += size
        since_draw += size
        st["words"][group(reg)] += size
        bmask = sum(0xFF << (8 * b) for b in range(4) if mask & (1 << b))
        for k, v in enumerate(vals):
            r = reg + k if seq else reg
            if r == 0x2C0:
                unif_idx, unif_f32, unif_buf = v & 0xFF, bool(v >> 31), []
                continue
            if 0x2C1 <= r <= 0x2C8:
                unif_buf.append(v)
                per = 4 if unif_f32 else 3
                if len(unif_buf) == per:
                    key = (unif_f32, tuple(unif_buf))
                    st["unif_vecs"] += 1
                    if unif.get(unif_idx) == key:
                        st["unif_redundant"] += 1
                        st["unif_red_words"] += per
                        st["unif_red_by"][(shbins.name(code_cur, entry), unif_idx)] += 1
                    unif[unif_idx] = key
                    unif_idx, unif_buf = unif_idx + 1, []
                continue
            if r == 0x2CB:
                code_buf = []
                continue
            if 0x2CC <= r <= 0x2D3:
                code_buf.append(v)
                code_cur = tuple(code_buf)
                continue
            if r == 0x2BA:
                entry = v & 0xFFFF
                st["prog_switches"] += 1
            if r in (0x22E, 0x22F):
                cfg2 = regs.get(0x205, 0)
                stride, nattr = (cfg2 >> 16) & 0xFF, cfg2 >> 28
                kind = "%s s%d a%d" % (shbins.name(code_cur, entry), stride, nattr)
                st["draws"][kind] += 1
                st["draw_words"][kind] += since_draw
                st["draw_verts"][kind] += regs.get(0x228, 0)  # NUMVERTICES: indices or vertices
                since_draw = 0
                continue
            if r == TEXCLEAR and (bmask & (1 << 16)) and (v & (1 << 16)):
                st["texclears"] += 1
            if r in PORTS or r in TRIGGERS:
                continue
            old = regs.get(r)
            new = ((old or 0) & ~bmask) | (v & bmask)
            if r in seen and old is not None and (old & bmask) == (v & bmask) and \
                    not (r == TEXCLEAR and (v & (1 << 16))):
                st["redundant"][r] += 1
                # a lone write: its 2 words; one value of a burst: 1 word
                st["redundant_words"][r] += 2 if len(vals) == 1 else 1
            regs[r] = new
            seen.add(r)
    return st


# (AI-assisted, micro-gpu) --kicks A B: the GPU state at every draw of two dumps, compared draw by
# draw. A draw's state: every register (byte masks applied; trigger writes left out), the float
# uniforms, the fixed attributes and LUT memories, and the vertex program the draw runs, resolved:
# the instructions from the entry point to END with each operand descriptor index replaced by the
# descriptor's value (so programs at another place in code memory, with their descriptors at other
# indices, count as the same program). Registers whose value only says where the program is
# (the entry point) are left out of the register compare for that reason.
MEMS = {  # data port -> (index register, words per entry)
    "vsh_f": (0x2C0, range(0x2C1, 0x2C9)), "gsh_f": (0x290, range(0x291, 0x299)),
    "vsh_code": (0x2CB, range(0x2CC, 0x2D4)), "vsh_desc": (0x2D5, range(0x2D6, 0x2DE)),
    "gsh_code": (0x29B, range(0x29C, 0x2A4)), "gsh_desc": (0x2A5, range(0x2A6, 0x2AE)),
    "light_lut": (0x1C5, range(0x1C8, 0x1D0)), "fog_lut": (0x0E6, range(0x0E8, 0x0F0)),
    "proc_lut": (0x0AF, range(0x0B0, 0x0B8)), "fixattr": (0x232, range(0x233, 0x236)),
}
PORT_OF = {r: k for k, (_, rs) in MEMS.items() for r in rs}
INDEX_OF = {ix: k for k, (ix, _) in MEMS.items()}
KICKS = {0x22E, 0x22F}
NO_STATE = TRIGGERS | {0x2BA}  # (the VSH entry point: in the resolved program)


def mad(op):
    return op >= 0x30


def resolve_program(code, desc, entry):
    if not code:
        return None  # (no upload seen yet in this dump)
    out = []
    pc = entry
    for _ in range(512):
        x = code.get(pc, 0)
        op = x >> 26
        if mad(op):
            out.append((x & ~0x1F, desc.get(x & 0x1F, 0)))
        elif op in (0x21, 0x22) or 0x20 <= op <= 0x2D:
            out.append((x, None))
        else:
            out.append((x & ~0x7F, desc.get(x & 0x7F, 0)))
        if op == 0x22:
            break
        pc += 1
    return tuple(out)


def kick_states(words):
    regs = {}
    mem = {k: {} for k in MEMS}
    ptr = {k: 0 for k in MEMS}
    f32 = {"vsh_f": False, "gsh_f": False}
    fbuf = {"vsh_f": [], "gsh_f": []}
    kicks = []
    i, n = 0, len(words)
    while i + 1 < n:
        param, hdr = words[i], words[i + 1]
        reg, mask, extra, seq = hdr & 0xFFFF, (hdr >> 16) & 0xF, (hdr >> 20) & 0xFF, hdr >> 31
        vals = [param] + list(words[i + 2: i + 2 + extra])
        i += 2 + extra + (extra & 1)
        bmask = sum(0xFF << (8 * b) for b in range(4) if mask & (1 << b))
        for k, v in enumerate(vals):
            r = reg + k if seq else reg
            if r in INDEX_OF:
                m = INDEX_OF[r]
                if m in f32:
                    f32[m], ptr[m], fbuf[m] = bool(v >> 31), v & 0xFF, []
                elif m == "fixattr":
                    ptr[m], fbuf.setdefault(m, [])[:] = v & 0xF, []
                elif m in ("vsh_code", "gsh_code", "vsh_desc", "gsh_desc"):
                    ptr[m] = v & 0xFFF
                elif m in ("light_lut", "proc_lut"):
                    ptr[m] = v & 0xFFFF  # (the table in bits 8-12, the entry in 0-7)
                else:
                    ptr[m] = v & 0xFF
                continue
            if r in PORT_OF:
                m = PORT_OF[r]
                if m in f32:
                    fbuf[m].append(v)
                    if len(fbuf[m]) == (4 if f32[m] else 3):
                        mem[m][ptr[m]] = (f32[m], tuple(fbuf[m]))
                        ptr[m], fbuf[m] = ptr[m] + 1, []
                elif m == "fixattr":
                    fbuf.setdefault(m, []).append(v)
                    if len(fbuf[m]) == 3:
                        mem[m][ptr[m]] = tuple(fbuf[m])
                        ptr[m], fbuf[m] = ptr[m] + 1, []
                else:
                    mem[m][ptr[m]] = v
                    ptr[m] += 1
                continue
            if r in KICKS:
                prog = resolve_program(mem["vsh_code"], mem["vsh_desc"],
                                       regs.get(0x2BA, 0) & 0xFFFF)
                st = {rr: vv for rr, vv in regs.items() if rr not in NO_STATE}
                kicks.append((r, st, {k: dict(mem[k]) for k in MEMS
                                      if k not in ("vsh_code", "vsh_desc")}, prog))
                continue
            if r in TRIGGERS:
                continue
            regs[r] = (regs.get(r, 0) & ~bmask) | (v & bmask)
    return kicks


def read_dump(f):
    """(frame, prologue words, frame words). "CMDE" files (shader_resident) start with the
    resident shader image as upload commands: state only, not the frame's words."""
    b = pathlib.Path(f).read_bytes()
    magic, frame, nw = struct.unpack("<3I", b[:12])
    if magic == 0x44444D43:
        return frame, (), struct.unpack("<%dI" % nw, b[12:12 + 4 * nw])
    if magic == 0x45444D43:
        npro = struct.unpack("<I", b[12:16])[0]
        pro = struct.unpack("<%dI" % npro, b[16:16 + 4 * npro])
        o = 16 + 4 * npro
        return frame, pro, struct.unpack("<%dI" % nw, b[o:o + 4 * nw])
    raise ValueError(f)


def compare_kicks(fa, fb, max_report):
    def load(f):
        frame, pro, words = read_dump(f)
        return frame, pro + words
    (frA, wa), (frB, wb) = load(fa), load(fb)
    ka, kb = kick_states(wa), kick_states(wb)
    print(f"A {fa}: frame {frA}, {len(wa)} words, {len(ka)} draws")
    print(f"B {fb}: frame {frB}, {len(wb)} words, {len(kb)} draws")
    # the draws paired up by kind, vertex layout and count (difflib: draws only one dump has, such as
    # particles of a frame that differ, are left out of the pairs and counted)
    sig = lambda k: (k[0], k[1].get(0x205, 0), k[1].get(0x228, 0))
    sm = difflib.SequenceMatcher(None, [sig(k) for k in ka], [sig(k) for k in kb], autojunk=False)
    pairs = [(a + j, b + j) for a, b, n in sm.get_matching_blocks() for j in range(n)]
    print(f"paired {len(pairs)} draws ({len(ka) - len(pairs)} of A and {len(kb) - len(pairs)} of B "
          f"without a partner)")
    same = 0
    reported = 0
    unknown_prog = 0
    by_what = collections.Counter()
    for ia, ib in pairs:
        x, y, d = ka[ia], kb[ib], ia
        diffs = []
        if x[0] != y[0]:
            diffs.append("draw kind")
        rk = set(x[1]) | set(y[1])
        for r in sorted(rk):
            if x[1].get(r) != y[1].get(r):
                diffs.append(f"{NAMES.get(r, hex(r))} {x[1].get(r, 0):#x}/{y[1].get(r, 0):#x}")
        for m in x[2]:
            if x[2][m] != y[2][m]:
                ks = sorted(set(x[2][m]) | set(y[2][m]))
                bad = [k for k in ks if x[2][m].get(k) != y[2][m].get(k)]
                diffs.append(f"{m}[{','.join(str(k) for k in bad[:6])}{'...' if len(bad) > 6 else ''}]")
        if x[3] is None or y[3] is None:
            unknown_prog += 1
        elif x[3] != y[3]:
            diffs.append(f"program ({len(x[3])} / {len(y[3])} instructions)")
        if diffs:
            if reported < max_report:
                print(f"  draw {d}: {'; '.join(diffs[:8])}")
            reported += 1
            for x_ in diffs:
                by_what[x_.split(" ")[0].split("[")[0]] += 1
        else:
            same += 1
    if by_what:
        print("  what differs (draws): " + ", ".join(f"{k} {v}" for k, v in by_what.most_common()))
    if unknown_prog:
        print(f"{unknown_prog} draws before the dump's first shader upload: program not compared")
    print(f"same state at {same} of {len(pairs)} paired draws ({reported} differ)"
          f"{'' if len(ka) == len(kb) == len(pairs) else ', NOT ALL DRAWS PAIRED'}")
    return 0 if same == len(ka) == len(kb) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shbin-dir", default=None)
    ap.add_argument("--kicks", action="store_true",
                    help="compare the GPU state at each draw of two dumps (A B)")
    ap.add_argument("--max-report", type=int, default=20)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    if a.kicks:
        if len(a.files) != 2:
            print("--kicks takes two files")
            return 2
        return compare_kicks(a.files[0], a.files[1], a.max_report)
    shbins = Shbins(a.shbin_dir)
    for f in a.files:
        try:
            frame, _, words = read_dump(f)
        except ValueError:
            print(f"{f}: not a cmd_dump file")
            continue
        nw = len(words)
        st = decode(words, shbins)
        draws = sum(st["draws"].values())
        print(f"== {f}: frame {frame}, {nw} words ({nw * 4 / 1024:.1f} KB), {draws} draws, "
              f"{st['prog_switches']} program switches, {st['texclears']} texture cache clears")
        for g_, w in st["words"].most_common():
            print(f"   {g_:28s} {w:7d} words  {100.0 * w / nw:5.1f}%")
        print("   draws (kind: count, words since the draw before, per draw; indices or vertices):")
        for k, c in st["draws"].most_common():
            print(f"     {k:40s} {c:5d} {st['draw_words'][k]:7d} {st['draw_words'][k] / c:7.1f} "
                  f"{st['draw_verts'][k]:8d}")
        red = sum(st["redundant_words"].values())
        print(f"   register writes that change nothing: {sum(st['redundant'].values())} "
              f"({red} words, {100.0 * red / nw:.1f}%)")
        for r, c in st["redundant"].most_common(14):
            print(f"     {NAMES.get(r, hex(r)):34s} {c:5d}  {st['redundant_words'][r]:6d} words")
        print(f"   float uniform vectors: {st['unif_vecs']}, the same value again: "
              f"{st['unif_redundant']} ({st['unif_red_words']:.0f} words)")
        for (prog, idx), c in st["unif_red_by"].most_common(8):
            print(f"     {prog:30s} c{idx:<3d} {c:5d}")


if __name__ == "__main__":
    sys.exit(main())
