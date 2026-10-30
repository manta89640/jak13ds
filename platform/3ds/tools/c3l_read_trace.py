#!/usr/bin/env python3
# The .c3l reads of a host gk run with CTR_READ_TRACE=FILE (docs/3ds-port/c3l_format.md).
#
#   c3l_read_trace.py FILE [--level NAME] [--jobs] [--list KIND:N]
#
# Per level load (L: load_file, parts jobs) the requests, the bytes requested, the bytes of blocks
# requested again, and the packed bytes of the blocks its pieces need, by section; then the totals
# per level and reader kind (S: stream jobs). --jobs: each stream job too. --list L:0 (or S:3):
# that reader's requests in order, with the piece before each.
import argparse
import bisect
import collections
import os
import struct


def layout(path):
    with open(path, "rb") as f:
        h = f.read(128)
        ver = struct.unpack_from("<I", h, 4)[0]
        (tod, _, _, ext, _, tex_tab, _, chunks, verts, _, idx, _, draws, _, texels, _, _, mmod, mvert,
         _, midx, _, mdraw, _, mblend, _) = struct.unpack_from("<26I", h, 24)
        starts = [(chunks, "chunks"), (verts, "vertices"), (idx, "indices"), (draws, "draws"),
                  (mmod, "merc models"), (mvert, "merc verts"), (midx, "merc indices"),
                  (mdraw, "merc draws"), (texels, "textures"), (tex_tab, "texture table")]
        starts += [(o, n) for o, n in ((tod, "tod"), (ext, "ext"), (mblend, "merc blend")) if o]
        starts.sort()
        blocks = []
        if ver >= 11:
            _, _, nb, _ = struct.unpack("<4sIII", f.read(16))
            for _ in range(nb):
                ro, rs, fo, ps = struct.unpack("<IIII", f.read(16))
                blocks.append((fo, ps & 0x0FFFFFFF, ro))
    return starts, blocks, os.path.getsize(path)


def section(starts, raw):
    i = bisect.bisect_right([s[0] for s in starts], raw) - 1
    return starts[i][1] if i >= 0 else "header"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--level")
    ap.add_argument("--jobs", action="store_true")
    ap.add_argument("--list")
    a = ap.parse_args()
    readers = []
    for line in open(a.trace):
        p = line.split()
        if not p:
            continue
        if p[0] == "o":
            readers.append({"kind": p[1], "path": p[2], "ops": []})
        elif p[0] in "rp" and readers:
            readers[-1]["ops"].append((p[0], int(p[-2]), int(p[-1])))
    layouts = {}
    seen_kind = collections.Counter()
    totals = collections.defaultdict(lambda: [0, 0, 0, 0])
    for rd in readers:
        name = os.path.basename(rd["path"])
        if not name.endswith(".c3l") or (a.level and a.level not in name):
            continue
        nth = seen_kind[(rd["kind"], name)]
        seen_kind[(rd["kind"], name)] += 1
        if rd["path"] not in layouts:
            layouts[rd["path"]] = layout(rd["path"])
        starts, blocks, size = layouts[rd["path"]]
        by_file = [b[0] for b in blocks]
        by_raw = [b[2] for b in blocks]

        def file_blocks(off, n):
            i = max(bisect.bisect_right(by_file, off) - 1, 0)
            while i < len(blocks) and blocks[i][0] < off + n:
                yield i
                i += 1

        def raw_blocks(off, n):
            i = max(bisect.bisect_right(by_raw, off) - 1, 0)
            while i < len(blocks) and blocks[i][2] < off + n:
                yield i
                i += 1

        read, need = collections.Counter(), collections.Counter()
        fetched, needed = set(), set()
        reqs = req_bytes = again = 0
        listing = a.list == f"{rd['kind']}:{nth}"
        last_piece = None
        for op, off, n in rd["ops"]:
            if op == "p":
                last_piece = (section(starts, off), off, n)
                for i in raw_blocks(off, n):
                    if i not in needed:
                        needed.add(i)
                        need[section(starts, blocks[i][2])] += blocks[i][1]
                continue
            reqs += 1
            req_bytes += n
            mine = 0
            for i in file_blocks(off, n):
                part = min(blocks[i][0] + blocks[i][1], off + n) - max(blocks[i][0], off)
                read[section(starts, blocks[i][2])] += part
                if i in fetched:
                    mine += part
                fetched.add(i)
            again += mine
            if listing:
                print(f"  {off:>10} {n / 1024:7.1f} KB, again {mine / 1024:6.1f} KB  "
                      f"(after piece {last_piece})")
        t = totals[(rd["kind"], name)]
        t[0] += reqs
        t[1] += req_bytes
        t[2] += again
        t[3] += sum(blocks[i][1] for i in needed)
        if rd["kind"] == "L" or a.jobs:
            print(f"{rd['kind']}:{nth} {name} (file {size // 1024} KB): {reqs} requests, "
                  f"{req_bytes // 1024} KB, {again // 1024} KB of it again, blocks needed "
                  f"{sum(blocks[i][1] for i in needed) // 1024} KB")
            if rd["kind"] == "L":
                for s in sorted(set(read) | set(need), key=lambda k: -read[k]):
                    print(f"    {s:14s} read {read[s] // 1024:7d} KB, needed {need[s] // 1024:7d} KB")
    print("totals: reader kind, level: requests, KB requested, KB again, KB of blocks needed")
    for (kind, name), t in sorted(totals.items()):
        print(f"  {kind} {name:16s} {t[0]:6d} {t[1] // 1024:9d} {t[2] // 1024:8d} {t[3] // 1024:8d}")


if __name__ == "__main__":
    main()
