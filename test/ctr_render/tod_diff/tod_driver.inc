// (AI-assisted) tod_diff: included in a namespace with one version's Tod struct and time of day
// code (TOD_LIGHT: its CtrLight.h, TOD_STRUCT, TOD_CODE): a level with the file's chunks, drawn
// every frame as draw_level does
#include TOD_LIGHT
struct CtrLevelData {
#include TOD_STRUCT
  std::string name = "citadel";
  std::vector<int> meshes;
  std::vector<u32> chunk_first_run;
  std::unique_ptr<Tod> tod;
  struct Ch {
    u32 vertex_count;
    u32 vis_run_count;
    u32 first_draw = 0, draw_count = 0;
  };
  std::vector<Ch> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<u32> draw_first_run, draw_run_count, chunk_unit;
  std::vector<c3l::VisRun> vis_runs;
  struct SU {
    u16 tod_slots = 0;
  };
  std::vector<SU> stream_units;
};
static GameVersion g_game_version = GameVersion::Jak1;
// a chunk's mesh memory, from TOD_ARENA (one block: tod_arm.py models its cache lines): n
// c3l::Vertex, or (TOD_SPLIT: the version keeps the colors on their own) n 12-byte records and
// then the n colors
struct MeshBuf {
  u8* p = nullptr;
  u32 n = 0;
  c3l::Vertex vertex(u32 i) const {
    c3l::Vertex v;
    if (TOD_SPLIT) {
      memcpy(&v, p + 12 * i, 12);
      memcpy(v.rgba, p + 12 * n + 4 * i, 4);
    } else {
      memcpy(&v, p + 16 * i, 16);
    }
    return v;
  }
};
static std::vector<MeshBuf>* g_vb = nullptr;
static size_t g_arena_used = 0;
static u8* arena_take(size_t bytes) {
  bytes = (bytes + 31) & ~(size_t)31;
  if (g_arena_used + bytes > sizeof(TOD_ARENA)) {
    abort();  // (a bigger TOD_ARENA_MB)
  }
  u8* p = TOD_ARENA + g_arena_used;
  g_arena_used += bytes;
  return p;
}
[[maybe_unused]] static void* ctr_gpu_mesh_vertices_inplace(int mesh, int* n) {
  auto& v = (*g_vb)[mesh];
  *n = (int)v.n;
  return v.p;
}
[[maybe_unused]] static void* ctr_gpu_mesh_colors_inplace(int mesh) {
  auto& v = (*g_vb)[mesh];
  return TOD_SPLIT ? v.p + 12 * v.n : nullptr;
}
// draw_level's heavy frames in Jak 1 (a version without tod_day_jump: in play too)
struct TodNoJump {};
template <typename... A>
TodNoJump tod_day_jump(const A&...) {
  return {};
}
template <typename J>
bool tod_skip_rule(bool heavy, u64 frame_idx, const J&) {
  if constexpr (std::is_same_v<J, TodNoJump>) {
    return heavy && (frame_idx % 2) != 0;
  } else {
    return heavy && (frame_idx % 2) != 0 && g_title;
  }
}
namespace {
#include TOD_CODE
}  // namespace

struct Sim {
  CtrLevelData lev;
  std::vector<MeshBuf> vb;
  std::vector<u32> visible;
  bool heavy = false;
  u64 verts = 0;

  // a chunk's vertices as the version's tod_scan / tod_mark_plain take them
  auto verts_of(size_t ci) {
#if TOD_SPLIT
    return TodVerts{vb[ci].p, (tod_word*)(vb[ci].p + 12 * vb[ci].n)};
#else
    return (const c3l::Vertex*)vb[ci].p;
#endif
  }

  // load_tod (v22, with alpha) and the chunk loop's time of day part
  void setup(const Dump& d, const std::vector<u32>& vis, int bright_step) {
    g_vb = &vb;
    ctr_settings().tod_pulse_step = bright_step;
    visible = vis;
    const u32 nc = d.nc, nm = d.nm;
    auto alpha_of = [](const c3l::TodColorA& c) {
      std::array<u8, 8> a;
      for (int p = 0; p < 8; p++) {
        a[p] = c.rgba[p][3];
      }
      return a;
    };
    auto rgb_mask = [](const c3l::TodColorA& c) {
      u32 m = 0;
      for (int p = 0; p < 8; p++) {
        if (c.rgba[p][0] | c.rgba[p][1] | c.rgba[p][2]) {
          m |= 1u << p;
        }
      }
      return m;
    };
    std::map<std::array<u8, 8>, u32> kind_of;
    u32 mask = 0, rgb_used[8] = {};
    for (u32 i = 0; i < nc; i++) {
      const auto& c = d.colors[i];
      const u32 m = rgb_mask(c);
      mask |= m;
      for (int p = 0; p < 8; p++) {
        rgb_used[p] += m >> p & 1;
      }
      kind_of.emplace(alpha_of(c), 0);
    }
    auto t = std::make_unique<CtrLevelData::Tod>();
    t->keep_alpha = 0;
    u32 day = 0;
    for (int p = 0; p < 8; p++) {
      if (rgb_used[p] * 4 > nc) {
        day |= 1u << p;
      }
    }
    for (auto& [a, k] : kind_of) {
      k = (u32)t->akinds.size();
      t->akinds.push_back(a);
      bool full = day != 0, zero = true;
      for (int p = 0; p < 8; p++) {
        full = full && (a[p] == 0x80 || (a[p] == 0 && !(day >> p & 1)));
        zero = zero && a[p] == 0;
        if (a[p]) {
          t->apal_mask |= 1u << p;
        }
      }
      t->akind_full.push_back(full && !zero);
    }
    t->aword.assign(t->akinds.size(), 0);
    auto kind_mask = [&](u32 k) {
      u32 m = 0;
      for (int p = 0; p < 8; p++) {
        if (t->akinds[k][p]) {
          m |= 1u << p;
        }
      }
      return m;
    };
    for (int p = 0; p < 8; p++) {
      if (mask & (1u << p)) {
        t->palettes[t->palette_count++] = (u8)p;
      }
    }
    t->track_mask = mask | t->apal_mask;
    const int step = bright_step;
    auto bright_of = [&](u32 v) { return step > 1 && v * (u32)(step - 1) >= 64; };
    t->color_count = nc;
    t->colors.resize((size_t)nc * t->palette_count * 3);
    t->color_mask.assign(nc + nm, 0);
    t->color_bright.assign(nc + nm, 0);
    if (t->akinds.size() > 1) {
      t->akind.assign(nc, 0);
    }
    u8* dst = t->colors.data();
    for (u32 j = 0; j < nc; j++) {
      const auto& c = d.colors[j];
      u32 m = 0;
      u8 bright = 0;
      for (u32 k = 0; k < t->palette_count; k++) {
        const u8* rgb = c.rgba[t->palettes[k]];
        memcpy(dst, rgb, 3);
        dst += 3;
        if (rgb[0] | rgb[1] | rgb[2]) {
          m |= 1u << t->palettes[k];
        }
        if (bright_of(std::max({rgb[0], rgb[1], rgb[2]}))) {
          bright |= (u8)(1u << t->palettes[k]);
        }
      }
      const u32 k = kind_of[alpha_of(c)];
      if (!t->akind.empty()) {
        t->akind[j] = (u8)k;
      }
      const u32 km = kind_mask(k);
      if (!t->akind_full[k] || !m) {
        m |= km;
      }
      if (!t->akind_full[k]) {
        for (int p = 0; p < 8; p++) {
          if (bright_of(c.rgba[p][3])) {
            bright |= (u8)(1u << p);
          }
        }
      }
      t->color_mask[j] = (u8)m;
      t->color_bright[j] = bright;
    }
    t->muls.assign(d.muls, d.muls + nm);
    for (u32 j = 0; j < nm; j++) {
      const auto& mu = t->muls[j];
      t->color_mask[nc + j] = t->color_mask[mu.color];
      const u8* src = t->colors.data() + (size_t)mu.color * t->palette_count * 3;
      const u32 f = std::max({mu.mul[0], mu.mul[1], mu.mul[2]});
      u8 bright = t->color_bright[mu.color];
      for (u32 k = 0; k < t->palette_count; k++, src += 3) {
        if (bright_of(std::max({src[0], src[1], src[2]}) * f / 128)) {
          bright |= (u8)(1u << t->palettes[k]);
        }
      }
      t->color_bright[nc + j] = bright;
    }
    {
      u32 used[8] = {};
      for (u32 i = 0; i < nc; i++) {
        for (u32 m = t->color_mask[i]; m; m &= m - 1) {
          used[__builtin_ctz(m)]++;
        }
      }
      for (int p = 0; p < 8; p++) {
        if ((t->track_mask >> p & 1) && used[p] * 4 <= nc) {
          t->light_mask |= 1u << p;
        }
      }
    }
    t->table.assign(nc + nm, 0);
    t->stamp.assign(nc + nm, 0);
    const size_t nch = d.chunks.size();
    t->chunk_group_first.assign(nch, 0);
    t->chunk_group_count.assign(nch, 0);
    t->chunk_min_ver.assign(nch, 0);
    t->chunk_min_dver.assign(nch, 0);
    t->chunk_light_ver.assign(nch, 0);
    t->chunk_mask.assign(nch, 0);
    t->chunk_bright.assign(nch, 0);
    t->chunk_scanned.assign(nch, 0);
    t->chunk_mesh.assign(nch, -1);
    t->pulse_known = 1u << 1;  // (a reload: citadel's palette 1 known to pulse)
    if (g_keep_alpha) {
      // (an older file: the vertices keep their alpha, the table has none)
      t->keep_alpha = 0xff000000u;
      t->akinds.clear();
      t->akind.clear();
      t->aword.clear();
      t->akind_full.clear();
    }
    tod_diff_load_extra(*t, 0);
    lev.tod = std::move(t);
    auto& T = *lev.tod;
    vb.resize(nch);
    lev.meshes.resize(nch);
    lev.chunk_first_run.assign(nch, 0);
    lev.chunks.resize(nch);
    std::vector<u8> drawn(nch, 0);
    for (u32 ci : vis) {
      drawn[ci] = 1;
    }
    for (size_t ci = 0; ci < nch; ci++) {
      const auto& c = d.chunks[ci];
      if (!drawn[ci]) {
        T.chunk_group_first[ci] = (u32)T.groups.size();
        T.chunk_scanned[ci] = 1;
        continue;
      }
      vb[ci].n = c.vertex_count;
      vb[ci].p = arena_take(c.vertex_count * sizeof(c3l::Vertex));
      for (u32 i = 0; i < c.vertex_count; i++) {
        if (TOD_SPLIT) {
          memcpy(vb[ci].p + 12 * i, &c.v[i], 12);
          memcpy(vb[ci].p + 12 * c.vertex_count + 4 * i, c.v[i].rgba, 4);
        } else {
          memcpy(vb[ci].p + 16 * i, &c.v[i], 16);
        }
      }
      lev.meshes[ci] = (int)ci;
      lev.chunk_first_run[ci] = (u32)T.run_group.size();
      lev.chunks[ci] = {c.vertex_count, c.ngroups, 0, 0};
      for (u32 g = 0; g < c.ngroups; g++) {
        T.run_group.push_back((u16)g);
      }
      T.chunk_group_first[ci] = (u32)T.groups.size();
      for (u32 g = 0; g < c.ngroups; g++) {
        T.groups.push_back(CtrLevelData::Tod::VisGroup{c.groups[2 * g], c.groups[2 * g + 1], 0});
      }
      T.chunk_group_count[ci] = (u16)c.ngroups;
      CtrLevelData::Tod::LightList lights;
      const u8 m = tod_scan(T, verts_of(ci), (int)c.vertex_count, &lights);
      if (m) {
        tod_mark_plain(T, (u32)ci, verts_of(ci), (int)c.vertex_count);
      }
      T.chunk_mask[ci] = m;
      T.chunk_scanned[ci] = m ? 2 : 1;
      if (!lights.verts.empty()) {
        T.chunk_lights[(u32)ci] = std::move(lights);
      }
      T.chunk_mesh[ci] = (int)ci;
    }
  }

  // one level draw (draw_level's time of day): w[p] = r, g, b, alpha weights
  void frame(const u8 w[8][4], u64 frame_idx, void (*mark)(int)) {
    g_vb = &vb;
    CtrBackgroundCamera cam{};
    for (int p = 0; p < 8; p++) {
      u32& a = (u32&)cam.itimes[p / 2][(p % 2) * 2];
      u32& b = (u32&)cam.itimes[p / 2][(p % 2) * 2 + 1];
      a = (u32)w[p][0] | (u32)w[p][1] << 16;
      b = (u32)w[p][2] | (u32)w[p][3] << 16;
    }
    auto& t = *lev.tod;
    mark(0);
    // (draw_level: Jak 1)
    const bool tod_skip = tod_skip_rule(heavy, frame_idx, tod_day_jump(t, cam));
    if (!tod_skip) {
      tod_update(t, lev.name, cam, frame_idx, g_title);
    }
    mark(1);
    TodPass ps;
    tod_diff_pass_frame(ps, frame_idx, 0);
    ps.pass = kTodRound;
    u64 drawn = 0;
    for (u32 ci : visible) {
      drawn += t.chunk_scanned[ci] == 2 ? lev.chunks[ci].vertex_count : 0;
    }
    ps.fresh_ok = drawn >= kTodFreshRatio * t.table.size();
    ps.spread = t.day_move_frame && frame_idx - t.day_move_frame < kTodSpreadFrames;
    ps.spread_left = std::max<s64>(kTodSpreadMin, (s64)(drawn / kTodSpreadFrames));
    auto shown_of = [&](u32 ci) -> const u8* {
      return g_shown ? g_shown + lev.chunk_first_run[ci] : nullptr;
    };
    for (u32 ci : visible) {
      verts += tod_recolor(lev, ci, shown_of(ci), ps);
    }
    mark(2);
    ps.pass = kTodLights;
    for (u32 ci : visible) {
      verts += tod_recolor(lev, ci, shown_of(ci), ps);
    }
    tod_finish();
    mark(3);
    heavy = ps.fresh_ok;
  }
};
