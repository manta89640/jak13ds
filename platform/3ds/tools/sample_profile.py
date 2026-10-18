#!/usr/bin/env python3
"""(AI-assisted) Sampling profiles of the host C-mode gk (macOS `sample`), mapped to GOAL names.
docs/3ds-port/jak2_perf.md. The C modules' functions are gc_fN in out/<game>/cmod/<hash>.so; the
names come from the "// name" comment above each definition in <hash>.c.

  sample_profile.py fold   SAMPLE.txt THREAD CMOD_DIR > x.folded   (THREAD e.g. "Thread_123: EE")
  sample_profile.py tree   x.folded [min_pct] [max_depth]          inclusive tree, glue frames hidden
  sample_profile.py est    x.folded MAC_MS [Rgoal Rc Rm2c] [N]     categories, kinds, top functions
                                                                   with an N3DS estimate per item
  sample_profile.py win    out.log                                 ms/frame per perf-mark window
                                                                   (1000 / fps of the no-sync lines)
Kinds: m2c = in a mips2c function (adapter/jalr glue under it included; GOAL callbacks it calls are
GOAL); c = C++ kernel / libc (memmove, typed adapters, timers); goal = goalc's C code and the C-mode
call glue. The ratios (Mac ms -> N3DS ms) default to 50 / 150 / 270, from Jak 1 (jak2_perf.md).
"""
import re, sys, os, collections
CMOD = "."
_names = {}
def so_names(h):
    if h in _names: return _names[h]
    m = {}; mod = h
    p = os.path.join(CMOD, h + ".c")
    if os.path.exists(p):
        prev = None
        with open(p, errors="replace") as f:
            for i, line in enumerate(f):
                if i == 0:
                    mm = re.match(r"// GOAL object (\S+)", line); mod = mm.group(1) if mm else h
                if line.startswith("// "): prev = line[3:].strip(); continue
                mm = re.match(r"static u64 (gc_f\d+)\(.*\{\s*$", line)
                if mm and prev: m[mm.group(1)] = prev
                if line.strip(): prev = None if not line.startswith("// ") else prev
    _names[h] = (mod, m); return _names[h]
def pretty(sym):
    mm = re.match(r"(\S+)\s+\(in ([0-9a-f]{16})\.so\)", sym)
    if mm:
        fn, h = mm.groups(); mod, m = so_names(h)
        return f"{m.get(fn, fn)} [{mod}]"
    mm = re.match(r"(.*?)\s+\(in (\S+)\)", sym)
    if mm:
        s = mm.group(1)
        s = re.sub(r"\(anonymous namespace\)::thunk<\d+u>\(.*", "thunk<>", s)
        s = re.sub(r"\(.*\)$", "", s) if "Mips2C" in s or "goalc" in s else s
        return s + (" [gk]" if mm.group(2) == "gk" else f" [{mm.group(2)}]")
    return sym
line_re = re.compile(r"^([ +!:|]*)(\d+) (.*?)(?:\s+\+ \d+)?\s+\[0x[0-9a-f,]+\].*$|^([ +!:|]*)(\d+) (.*)$")
def parse(path, thread):
    nodes = []  # (depth, count, name)
    on = False
    for line in open(path, errors="replace"):
        if line.startswith("    ") and re.match(r"^    \d+ Thread_", line):
            on = thread in line; continue
        if line.startswith("Total number"): break
        if not on: continue
        mm = line_re.match(line.rstrip("\n"))
        if not mm: continue
        if mm.group(2): ind, c, n = mm.group(1), mm.group(2), mm.group(3)
        else: ind, c, n = mm.group(4), mm.group(5), mm.group(6)
        nodes.append((len(ind), int(c), pretty(n.strip())))
    return nodes
def analyze(nodes):
    selfc = collections.Counter(); incl = collections.Counter()
    stack = []  # (depth, name, count, childsum)
    total = 0
    # compute self = count - sum(direct children)
    childsum = [0]*len(nodes)
    par = [-1]*len(nodes); st = []
    for i,(d,c,n) in enumerate(nodes):
        while st and nodes[st[-1]][0] >= d: st.pop()
        par[i] = st[-1] if st else -1
        if par[i] >= 0: childsum[par[i]] += c
        st.append(i)
    # inclusive without double counting recursion: count names on the path once
    path = []
    for i,(d,c,n) in enumerate(nodes):
        s = c - childsum[i]
        if s > 0: selfc[n] += s
    # inclusive: name counted at outermost occurrence
    st = []
    for i,(d,c,n) in enumerate(nodes):
        while st and nodes[st[-1]][0] >= d: st.pop()
        if n not in [nodes[j][2] for j in st]: incl[n] += c
        st.append(i)
    return selfc, incl, par, childsum
def fold(path, thread):
    nodes = parse(path, thread)
    cs = [0] * len(nodes); st = []
    for i, (d, c, n) in enumerate(nodes):
        while st and nodes[st[-1]][0] >= d: st.pop()
        if st: cs[st[-1]] += c
        st.append(i)
    st = []; out = {}
    for i, (d, c, n) in enumerate(nodes):
        while st and nodes[st[-1]][0] >= d: st.pop()
        st.append(i)
        s = c - cs[i]
        if s > 0:
            key = ";".join(nodes[j][2].replace(";", ",") for j in st)
            out[key] = out.get(key, 0) + s
    return out
TREE_GLUE = re.compile(r"^(\?\?\?|thunk<>|Mips2C::mips2c_goalc_adapter|Mips2C::ExecutionContext::jalr|goalc_call|jak2::goalc_k_|jak2::\(anonymous namespace\)::|thread_start|_pthread_start|std::__thread_proxy|bootstrap_thread_func|\(anonymous namespace\)::ee_runner|jak2::goal_main|<deduplicated_symbol>|___chkstk)")
def clean(path):
    out = []
    for f in path.split(";"):
        if TREE_GLUE.match(f): continue
        if out and out[-1] == f: continue
        out.append(f)
    return out
def load(fn):
    rows = []
    for line in open(fn):
        k, v = line.rstrip("\n").rsplit(" ", 1); rows.append((k, int(v)))
    return rows
def tree_main(fn, minp=1.0, maxd=12):
    rows = load(fn)
    tot = sum(v for k, v in rows if not IDLE.search(k))
    t = collections.defaultdict(int)
    for k, v in rows:
        if IDLE.search(k): continue
        p = clean(k)
        for i in range(1, min(len(p), maxd) + 1): t[tuple(p[:i])] += v
    print(f"logic samples {tot}")
    def walk(prefix, depth):
        kids = sorted([(v, k) for k, v in t.items() if len(k) == len(prefix) + 1 and k[:len(prefix)] == prefix], reverse=True)
        for v, k in kids:
            if 100 * v / tot < minp: continue
            print(f"{'  ' * depth}{100 * v / tot:5.1f}% {k[-1]}")
            walk(k, depth + 1)
    walk((), 0)
IDLE = re.compile(r"__semwait_signal|sleep_for|nanosleep|clock_get_time")
GLUE = re.compile(r"^(\?\?\?|thunk<>|Mips2C::mips2c_goalc_adapter|Mips2C::ExecutionContext::jalr|goalc_call|jak[12]::goalc_k_|jak[12]::\(anonymous namespace\)::|thread_start|_pthread_start|std::__thread_proxy|bootstrap_thread_func|\(anonymous namespace\)::ee_runner|jak[12]::goal_main|jak[12]::Kernel|<deduplicated_symbol>|___chkstk|kernel_function_detail::|Mips2C:: \[gk\]$)")
# (category, regex on a frame) ; deepest matching frame decides
RULES = [
 ("kernel/process", r"\[(gkernel|gkernel-h)\]|jak[12]::goalc_k_thread|thread-suspend|thread-resume"),
 ("misc engine (level/entity/settings/res)", r"\[(level|entity|settings|res|load-dgo|loader|game-info|task-control|game-task|actor-link-h|actor-hash|scene|cam-master)\]"),
 ("pc port (pckernel/discord/subtitles)", r"\[(pckernel|pckernel-common|pckernel-impl|subtitle2|speedruns|pc-anim-util)\]"),
 ("text/hud/menu", r"\[(text|font|hud|hud-classes|progress|minimap|bigmap|menu|default-menu)\]|draw_string_asm|get_string_length"),
 ("time-of-day/lights", r"\[(time-of-day|lights|light-hash|mood|mood-tables|mood-funcs|mood-funcs2|weather-part)\]|add_light_sphere|light_hash"),
 ("sound", r"\[(gsound|sound|speech|ambient|gsound-h)\]"),
 ("camera", r"\[(camera|cam-states|cam-interface|cam-update|cam-layout|cam-debug|cam-combiner|cam-states-dbg|cam-start)\]"),
 ("target (Jak)", r"\[(target|target2|logic-target|target-util|target-handler|target-anim|target-death|target-gun|gun-util|gun-states|board-states|target-board|darkjak|target-darkjak|target-swim|target-carry|target-turret|sidekick|gun|board|board-util|target-h|collide-reaction-target)\]"),
 ("joints/bones (anim)", r"\[(joint|bones|process-drawable|joint-mod|aligner|joint-exploder|anim-tester|eye)\]|cspace_parented|bones_mtx|decompress_|normalize_frame|clear_frame_acc|calc_animation"),
 ("foreground draw (merc/dma)", r"\[(foreground|drawable|merc|emerc|generic-merc|texture|draw-node|vu1-user|ripple|blerc|merc-blend-shape|prim)\]|foreground_|generic_|mercneric|adgif_shader|blerc|ripple_"),
 ("shadow (CPU volumes)", r"\[(shadow-cpu|shadow-vu1|shadow)\]|shadow_"),
 ("sky", r"\[(sky-tng|sky|sky-h)\]|sky_work|sky_quad|sky_tri|large_polygon\b|clip_polygon|set_sky|set_tex_offset"),
 ("background draw (tfrag/tie/shrub)", r"\[(tfrag|tie|shrubbery|background|subdivide|prototype|bsp|vis)\]"),
 ("ocean", r"\[(ocean|ocean-vu0|ocean-texture|ocean-mid|ocean-near|ocean-transition)\]|ocean_|_ocean"),
 ("particles", r"\[(sparticle|sparticle-launcher|generic-obs|part-tracker)\]|sp_process|sp_launch|particle_adgif|sparticle_motion"),
 ("collision", r"\[(collide|collide-cache|collide-shape|collide-shape-rider|collide-mesh|collide-probe|collide-func|collide-hash|collide-edge-grab|collide-touch|collide-frag|collide-target|find-nearest|los-control)\]|collide_|fill_bg_using|moving_sphere|method_\d+_collide"),
 ("spatial hash (actor/avoid queries)", r"\[(spatial-hash|actor-hash)\]|grid_hash|sphere_hash|spatial_hash"),
 ("nav-mesh/nav-control", r"\[(nav-mesh|nav-control|nav-engine|nav-graph|nav-mesh-h)\]|nav_state|nav_mesh|nav_engine|nav_dma|method_39_nav_state"),
 ("enemy/nav-enemy framework", r"\[(enemy|nav-enemy|enemy-states|nav-enemy-states|enemy-h|nav-enemy-h)\]"),
 ("traffic manager/engine", r"\[(traffic-engine|traffic-manager|traffic-util|traffic-h|citywide|ctywide-obs|ctywide-part|ctywide-speech|ctywide-tasks|city-util)\]"),
 ("citizens/guards AI", r"\[(citizen|civilian|citizen-norm|citizen-chick|citizen-fat|citizen-enemy|guard|crimson-guard-level|crimson-guard|crimson-guard-hover|metalhead-predator|metalhead-flitter|metalhead-grunt|guard-conversation|citizen-h|guard-states|guard-tazer|guard-rifle|guard-grenade|ctyport-obs|ctyport-part|ctysluma-part|ctyslumb-part|ctyinda-part|ctyindb-part|searchlight|baron-statue)\]"),
 ("vehicles AI/physics", r"\[(vehicle|vehicle-states|vehicle-control|vehicle-effects|vehicle-util|vehicle-hud|vehicle-guard|vehicle-rider|vehicle-h|vehicle-part|bikea|bikeb|bikec|bike|car|cara|carb|carc|hellcat|guard-bike|test-bike|test-car|turret|pilot-states|rigid-body|rigid-body-h|rigid-body-queue)\]"),
 ("math helpers (vector/trig/geometry)", r"\[(vector|vector-h|trigonometry|trigonometry-h|geometry|matrix|matrix-h|quaternion|transformq|math|transform|euler|bounding-box|gstring|gstate|random)\]"),
]
RULES = [(n, re.compile(r)) for n,r in RULES]
HELPER = re.compile(r"\[(vector|vector-h|trigonometry|trigonometry-h|geometry|matrix|matrix-h|quaternion|transformq|math|transform|euler|bounding-box|gstring|gcommon|random|libsystem_platform\.dylib|libsystem_c\.dylib|libcommon\.dylib)\]$|read_ee_timer|kernel_function_detail")
def classify(frames):
    for f in reversed(frames):
        if GLUE.match(f) or HELPER.search(f): continue
        for n,r in RULES:
            if r.search(f): return n
    # nothing matched: the deepest non-glue frame's module
    for f in reversed(frames):
        if not GLUE.match(f):
            m = re.search(r"\[([^\]]+)\]$", f); return "other: " + (m.group(1) if m else f)
    return "other: glue-only"
def est_main(argv):
    rows = load(argv[0]); mac = float(argv[1])
    R = dict(goal=50.0, c=150.0, m2c=270.0)
    if len(argv) >= 5: R = dict(goal=float(argv[2]), c=float(argv[3]), m2c=float(argv[4]))
    ntop = int(argv[5]) if len(argv) >= 6 else 40
    LIB = re.compile(r"\[(libsystem_\w+\.dylib|libc\+\+\S*|libcommon\.dylib)\]$|kernel_function_detail|read_ee_timer|Timer::")
    def kind_owner(fr):
        # nearest non-glue frame from the leaf
        leaf = fr[-1]
        for f in reversed(fr):
            if f.startswith("Mips2C::jak2::"): return "m2c", f
            if GLUE.match(f): continue
            if LIB.search(f): continue
            return ("goal" if not LIB.search(leaf) else "c"), f
        return "goal", fr[-1]
    tot = sum(v for k, v in rows if not IDLE.search(k))
    cat = collections.defaultdict(lambda: collections.Counter()); fn = collections.defaultdict(lambda: collections.Counter())
    fncat = {}
    for k, v in rows:
        if IDLE.search(k): continue
        fr = k.split(";"); kd, own = kind_owner(fr); c = classify(fr)
        cat[c][kd] += v; fn[own][kd] += v; fncat.setdefault(own, collections.Counter())[c] += v
    def ms(cnt): return mac * cnt / tot
    def n3(cn): return sum(ms(cn[k]) * R[k] for k in cn)
    print(f"Mac {mac:.3f} ms/frame, {tot} samples; R = {R}")
    T = sum(n3(c) for c in cat.values())
    print(f"N3DS estimate total {T:.1f} ms")
    kt = collections.Counter()
    for cn in cat.values(): kt.update(cn)
    print("kinds: " + ", ".join(f"{k} {100*v/tot:.1f}% Mac {1000*ms(v):.0f}us -> {ms(v)*R[k]:.1f} ms" for k, v in kt.most_common()))
    print(f"{'category':40s} {'Mac us':>7s} {'%':>5s} {'m2c%':>5s} {'N3DS ms':>8s}")
    for c, cn in sorted(cat.items(), key=lambda x: -n3(x[1])):
        s = sum(cn.values())
        print(f"{c:40s} {1000*ms(s):7.1f} {100*s/tot:5.1f} {100*cn['m2c']/s:5.0f} {n3(cn):8.2f}")
    print("== top functions (self, glue charged to nearest named frame) ==")
    for f, cn in sorted(fn.items(), key=lambda x: -n3(x[1]))[:ntop]:
        s = sum(cn.values()); c = fncat[f].most_common(1)[0][0]
        print(f"{100*s/tot:5.1f}% {1000*ms(s):6.1f}us {n3(cn):6.2f}ms  {f}  <{c}>")

def win_main(path):
    cur = None; first = False; vals = {}
    for line in open(path, errors="replace"):
        m = re.search(r"perf-mark (\S+)\s*$", line)
        if m:
            t = m.group(1); cur = None if t.endswith("-end") else t; first = True; continue
        m = re.search(r"perf: +([\d.]+) fps +logic", line)
        if m and cur:
            fps = float(m.group(1))
            if fps < 100: continue  # a 30 Hz line
            if first: first = False; continue  # partly before the window
            vals.setdefault(cur, []).append(1000.0 / fps)
    for k, v in vals.items():
        v2 = sorted(v)
        print(f"{k:12s} n={len(v):3d} mean {sum(v)/len(v):.3f} ms  median {v2[len(v)//2]:.3f}  min {v2[0]:.3f} max {v2[-1]:.3f}")

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "fold":
        CMOD = sys.argv[4]
        for k, v in fold(sys.argv[2], sys.argv[3]).items(): print(k, v)
    elif cmd == "tree":
        tree_main(sys.argv[2], *(float(x) if i == 0 else int(x) for i, x in enumerate(sys.argv[3:5])))
    elif cmd == "win":
        win_main(sys.argv[2])
    elif cmd == "est":
        est_main(sys.argv[2:])
    else:
        sys.exit(__doc__)
