// (AI-assisted) tod_diff: two versions of CtrLevel.cpp's time of day code (run.sh copies them:
// "ref", usually the commit before a change, and "neu", the working tree) on one level file and a
// weight trace, in lockstep; every vertex of the chunks drawn compared after every level draw.
//   tod_diff <level.tod> <trace> <frames> <stride> <pulse_step> [vis] [keep] [title] [title-half]
// stride: every stride-th chunk drawn; vis: runs shown and hidden at random (both versions alike);
// keep: an older file's table (no alpha, the vertices keep theirs); title: the title screen (no
// hold, fast day); title-half: title for the first half of the frames only; exact: every vertex of
// neu's chunks also against the PC's blend of the frame's weights (a trace without day steps:
// lightning, lamps).
// Built for ARM too (tod_arm.cpp): ARM instructions per level draw of one version.
#include <cstdio>

#include "tod_shim.h"

bool g_title = false;
bool g_keep_alpha = false;
const u8* g_shown = nullptr;

// the fields one version has and the other may not
template <typename T>
auto tod_diff_load_extra(T& t, int) -> decltype(t.cver, void()) {
  t.cver.assign(t.color_count, 0);
}
template <typename T>
void tod_diff_load_extra(T&, long) {}
template <typename P>
auto tod_diff_pass_frame(P& ps, u64 f, int) -> decltype(ps.frame = f, void()) {
  ps.frame = f;
}
template <typename P>
void tod_diff_pass_frame(P&, u64, long) {}

// the bytes the version's cache flushes cover (tod_arm.py reads it per level draw)
extern "C" {
u32 g_tod_flush_bytes = 0;
void tod_flush_note(u32 bytes) {
  g_tod_flush_bytes += bytes;
}
}

// each version's chunk meshes in one block (run.sh: REF_SPLIT / NEU_SPLIT, the version keeps the
// colors on their own)
#ifndef TOD_ARENA_MB
#define TOD_ARENA_MB 24
#endif
alignas(32) u8 g_arena_ref[TOD_ARENA_MB << 20];
alignas(32) u8 g_arena_neu[TOD_ARENA_MB << 20];

namespace ref {
#define TOD_LIGHT "ref.light.h"
#define TOD_STRUCT "ref.struct.inc"
#define TOD_CODE "ref.code.inc"
#define TOD_ARENA g_arena_ref
#define TOD_SPLIT REF_SPLIT
#include "tod_driver.inc"
#undef TOD_LIGHT
#undef TOD_STRUCT
#undef TOD_CODE
#undef TOD_ARENA
#undef TOD_SPLIT
}  // namespace ref
namespace neu {
#define TOD_LIGHT "neu.light.h"
#define TOD_STRUCT "neu.struct.inc"
#define TOD_CODE "neu.code.inc"
#define TOD_ARENA g_arena_neu
#define TOD_SPLIT NEU_SPLIT
#include "tod_driver.inc"
#undef TOD_LIGHT
#undef TOD_STRUCT
#undef TOD_CODE
#undef TOD_ARENA
#undef TOD_SPLIT
}  // namespace neu

#ifdef TOD_PIXELS
// pixels: the chunks drawn, of both versions, through the host soft renderer from their own mesh
// layout, every 25th level draw (tod_pixels.cpp)
#include "tod_pixels.h"
template <typename B>
static std::vector<unsigned char> draw_pixels(const Dump& d, const std::vector<u32>& vis,
                                              const std::vector<B>& vb, bool split) {
  std::vector<TodPxChunk> cs;
  for (u32 ci : vis) {
    const auto& c = d.chunks[ci];
    cs.push_back({c.bsphere, c.origin, c.scale, c.indices, c.nindices, vb[ci].p, vb[ci].n});
  }
  return tod_draw_pixels(cs.data(), cs.size(), split);
}
#endif

extern "C" __attribute__((noinline)) void sim_mark(int phase) {
  asm volatile("" ::"r"(phase) : "memory");
}
static void no_mark(int) {}

u32 g_px_frames = 0, g_px_bad_frames = 0, g_px_bad = 0, g_px_lit = 0;

enum : int {
  kRunRef = 1,
  kRunNeu = 2,
  kCompare = 4,
  kMarkRef = 8,
  kMarkNeu = 16,
  kVis = 32,
  kKeep = 64,
  kTitle = 128,
  kTitleHalf = 256,
  kExact = 512,
};

// vertices of the chunks drawn whose rgb isn't the PC's blend (interp_time_of_day_slow) of w
template <typename B>
static u32 inexact_vertices(const Dump& d, const std::vector<u32>& vis, const std::vector<B>& vb,
                            const u8 w[8][4]) {
  u32 bad = 0;
  for (u32 ci : vis) {
    for (u32 i = 0; i < vb[ci].n; i++) {
      const c3l::Vertex v = vb[ci].vertex(i);
      const u32 idx = (u16)v.pad;
      if (idx >= d.nc + d.nm) {
        continue;
      }
      const u32 col = idx < d.nc ? idx : d.muls[idx - d.nc].color;
      bool same = true;
      for (int c = 0; c < 3; c++) {
        u32 s = 0;
        for (int p = 0; p < 8; p++) {
          s += w[p][c] * d.colors[col].rgba[p][c];
        }
        u32 x = std::min<u32>(255, s >> 6);
        if (idx >= d.nc) {
          x = std::min<u32>(255, (x * d.muls[idx - d.nc].mul[c] + 64) >> 7);
        }
        same = same && v.rgba[c] == x;
      }
      bad += !same;
    }
  }
  return bad;
}

// out: [0] vertices drawn, [1] frames that differ, [2] vertices that differ, [3] / [4] vertices
// written by ref / neu, [5] frames with neu's colors not the blend (exact), [6] such vertices
extern "C" int sim_main(const u8* dump, const u32* trace, int nframes, int mode, int stride,
                        int step, u32* out) {
  Dump d = parse_dump(dump);
  std::vector<u32> vis;
  u64 drawn = 0;
  for (u32 ci = 0; ci < d.chunks.size(); ci++) {
    if (d.chunks[ci].kind != c3l::CHUNK_KIND_VISTA && ci % (u32)stride == 0) {
      vis.push_back(ci);
      drawn += d.chunks[ci].vertex_count;
    }
  }
  out[0] = (u32)drawn;
  g_title = (mode & kTitle) != 0;
  g_keep_alpha = (mode & kKeep) != 0;
  std::vector<u8> shown;
  u32 rng = 12345;
  if (mode & kVis) {
    u32 runs = 0;
    for (const auto& c : d.chunks) {
      runs += c.ngroups;
    }
    shown.assign(runs, 1);
    g_shown = shown.data();
  }
  static ref::Sim r;
  static neu::Sim n;
  if (mode & kRunRef) {
    r.setup(d, vis, step);
  }
  if (mode & kRunNeu) {
    n.setup(d, vis, step);
  }
  u32 bad_frames = 0, bad_verts = 0, off_frames = 0, off_verts = 0;
  for (int f = 0; f < nframes; f++) {
    const u32* row = trace + f * 33;
    u8 w[8][4];
    for (int p = 0; p < 8; p++) {
      for (int c = 0; c < 4; c++) {
        w[p][c] = (u8)row[1 + p * 4 + c];
      }
    }
    for (auto& s : shown) {
      rng = rng * 1664525u + 1013904223u;
      s ^= (rng >> 8) % 16 == 0;
    }
    if ((mode & kTitleHalf) && f == nframes / 2) {
      g_title = !g_title;
    }
    if (mode & kRunRef) {
      r.frame(w, row[0], (mode & kMarkRef) ? sim_mark : no_mark);
    }
    if (mode & kRunNeu) {
      n.frame(w, row[0], (mode & kMarkNeu) ? sim_mark : no_mark);
    }
    if ((mode & (kRunRef | kRunNeu | kCompare)) == (kRunRef | kRunNeu | kCompare)) {
      u32 bad = 0;
      for (u32 ci : vis) {
        for (u32 i = 0; i < r.vb[ci].n; i++) {
          const c3l::Vertex a = r.vb[ci].vertex(i), b = n.vb[ci].vertex(i);
          bad += memcmp(&a, &b, sizeof(c3l::Vertex)) != 0;
        }
      }
      bad_frames += bad != 0;
      bad_verts += bad;
    }
#ifdef TOD_PIXELS
    if ((mode & (kRunRef | kRunNeu | kCompare)) == (kRunRef | kRunNeu | kCompare) && f % 25 == 0) {
      const auto pr = draw_pixels(d, vis, r.vb, REF_SPLIT != 0);
      const auto pn = draw_pixels(d, vis, n.vb, NEU_SPLIT != 0);
      u32 px = 0, lit = 0;
      for (size_t i = 0; i + 3 < pr.size() && pr.size() == pn.size(); i += 4) {
        px += memcmp(&pr[i], &pn[i], 4) != 0;
        lit += (pr[i] | pr[i + 1] | pr[i + 2]) != 0;
      }
      g_px_frames++;
      g_px_bad += px;
      g_px_lit += lit;
      g_px_bad_frames += px != 0 || pr.size() != pn.size();
    }
#endif
    if ((mode & (kRunNeu | kExact)) == (kRunNeu | kExact)) {
      const u32 off = inexact_vertices(d, vis, n.vb, w);
      off_frames += off != 0;
      off_verts += off;
    }
  }
  out[5] = off_frames;
  out[6] = off_verts;
  out[1] = bad_frames;
  out[2] = bad_verts;
  out[3] = (u32)r.verts;
  out[4] = (u32)n.verts;
  return 0;
}

#ifndef SIM_ARM
int main(int argc, char** argv) {
  if (argc < 6) {
    fprintf(stderr,
            "usage: tod_diff <level.tod> <trace> <frames> <stride> <pulse_step> [vis] [keep] "
            "[title] [title-half]\n");
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    return 2;
  }
  fseek(f, 0, SEEK_END);
  std::vector<u8> dump(ftell(f));
  fseek(f, 0, SEEK_SET);
  if (fread(dump.data(), 1, dump.size(), f) != dump.size()) {
    return 2;
  }
  fclose(f);
  std::vector<u32> tr;
  FILE* t = fopen(argv[2], "r");
  if (!t) {
    return 2;
  }
  u32 x;
  while (fscanf(t, "%u", &x) == 1) {
    tr.push_back(x);
  }
  fclose(t);
  int mode = kRunRef | kRunNeu | kCompare;
  std::string opts;
  for (int i = 6; i < argc; i++) {
    const std::string a = argv[i];
    mode |= a == "vis"          ? kVis
            : a == "keep"       ? kKeep
            : a == "title"      ? kTitle
            : a == "title-half" ? kTitleHalf
            : a == "exact"      ? kExact
                                : 0;
    opts += " " + a;
  }
  const int nframes = std::min<int>(atoi(argv[3]), (int)(tr.size() / 33));
  u32 out[8] = {};
  sim_main(dump.data(), tr.data(), nframes, mode, atoi(argv[4]), atoi(argv[5]), out);
  auto base = [](const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
  };
  printf("%s %s: %d frames, stride %s, pulse step %s%s: %u vertices drawn; %u frames, %u vertices "
         "differ; written ref %u, neu %u",
         base(argv[1]), base(argv[2]), nframes, argv[4], argv[5], opts.c_str(), out[0], out[1],
         out[2], out[3], out[4]);
  if (mode & kExact) {
    printf("; neu not the blend: %u frames, %u vertices", out[5], out[6]);
  }
  if (g_px_frames) {
    printf("; pixels: %u frames drawn (%u lit pixels), %u frames, %u pixels differ", g_px_frames,
           g_px_lit, g_px_bad_frames, g_px_bad);
  }
  printf("\n");
  return out[1] || out[5] || g_px_bad_frames ? 1 : 0;
}
#endif
