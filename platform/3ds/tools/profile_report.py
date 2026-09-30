#!/usr/bin/env python3
# (AI-assisted) turn profile_emu.sh samples into a per-function report.
# usage: profile_report.py gdb.txt gk.elf <csrc dir>
import bisect, collections, os, re, subprocess, sys

gdb_txt, elf, csrc = sys.argv[1:4]
readelf = os.path.join(os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM"), "bin", "arm-none-eabi-readelf")
syms = []  # (addr, size, name, file)
cur_file = ""
for line in subprocess.run([readelf, "-sW", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) < 8:
        continue
    if p[3] == "FILE":
        cur_file = p[7]
    elif p[3] == "FUNC" and p[1] != "00000000":
        syms.append((int(p[1], 16) & ~1, int(p[2]), p[7], cur_file if p[4] == "LOCAL" else ""))
syms.sort()
addrs = [s[0] for s in syms]

goal_names = {}  # (module.c, gc_fN) -> GOAL name
def goal_name(file, fn):
    key = (file, fn)
    if key not in goal_names:
        path = os.path.join(csrc, file)
        name = fn
        if os.path.exists(path):
            text = open(path, errors="replace").read()
            m = re.search(r"// ([^\n]*)\n(?:// [^\n]*\n)?static \w+ %s\(" % re.escape(fn), text)
            if m:
                name = m.group(1)
        goal_names[key] = name
    return goal_names[key]

def lookup(pc):
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0:
        return "?"
    a, size, name, file = syms[i]
    if size and pc >= a + size + 64:
        return "? 0x%x" % pc
    if re.match(r"gc_f\d+$", name) and file.endswith(".c"):
        return "GOAL %s: %s" % (file[:-2], goal_name(file, name))
    return name + (" [%s]" % file if file else "")

per_thread = collections.defaultdict(collections.Counter)
n = 0
for line in open(gdb_txt, errors="replace"):
    if line.startswith("==SAMPLE=="):
        n += 1
    m = re.match(r"T (\d+) ([0-9a-f]+)", line)
    if m:
        per_thread[int(m.group(1))][lookup(int(m.group(2), 16))] += 1

print("%d samples" % n)
for t, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
    total = sum(c.values())
    busy = total - sum(v for k, v in c.items() if re.search(r"svc|Wait|Sleep|__libc|wait", k))
    print("\n== thread %d: %d samples (%d not waiting)" % (t, total, busy))
    for name, v in c.most_common(40):
        print("%6.1f%%  %s" % (100.0 * v / total, name))
    cats = collections.Counter()
    for name, v in c.items():
        if name.startswith("GOAL "):
            cats["GOAL code"] += v
        elif "Mips2C" in name or "execute" in name and "Mips2C" in name:
            cats["mips2c"] += v
        elif name.startswith("_ZN6Mips2C"):
            cats["mips2c"] += v
        elif re.search(r"svc|Wait|Sleep|wait", name):
            cats["waiting"] += v
        else:
            cats["runtime/other"] += v
    print("  by category: " + ", ".join("%s %.1f%%" % (k, 100.0 * v / total) for k, v in cats.most_common()))
