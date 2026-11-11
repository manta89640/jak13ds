// (AI-assisted) Exercise the converter's actual internal passes with small synthetic assets.
#define main ctr_level_converter_main
#include "tools/ctr_level_converter/main.cpp"
#undef main

#include "check.h"

void run_vram_tests();
void run_citadel_tests();
void run_cpu_bench_tests();  // (AI-assisted, cpubench)

namespace {
void test_resize() {
  ctr_tex::Image src{16, 16, std::vector<u8>(16 * 16 * 4, 0)};
  // One red opaque texel and three transparent black texels per destination texel.
  for (int y = 0; y < 16; y += 2) {
    for (int x = 0; x < 16; x += 2) {
      src.at(x, y)[0] = src.at(x, y)[3] = 255;
    }
  }
  const auto dst = ctr_tex::resize_image(src, 8, 8, {});
  check(dst.at(0, 0)[0] == 255 && dst.at(0, 0)[3] == 64,
        "resizing must not blend transparent black into visible red");

  // 128/1024 source texels pass: eight of 64 destination texels should pass too.
  // All uncorrected box averages are below aref 76 (64, 32 or 0).
  ctr_tex::Image mask{32, 32, std::vector<u8>(32 * 32 * 4, 0)};
  for (int cell = 0; cell < 64; cell++) {
    const int samples = cell < 8 ? 4 : cell < 56 ? 2 : 0;
    for (int k = 0; k < samples; k++) {
      mask.at((cell % 8) * 4 + k, (cell / 8) * 4)[3] = 255;
    }
  }
  ctr_tex::MipOptions mo;
  mo.preserve_coverage = true;
  const auto cutout = ctr_tex::resize_image(mask, 8, 8, mo);
  int pass = 0;
  for (size_t i = 3; i < cutout.rgba.size(); i += 4) {
    pass += cutout.rgba[i] >= mo.alpha_ref;
  }
  check(pass == 8, "initial resize must preserve source cutout coverage");

  tfrag3::Texture tex{};
  tex.w = tex.h = 16;
  tex.data.assign(256, 0);
  for (int y = 0; y < 16; y += 2) {
    for (int x = 0; x < 16; x += 2) {
      tex.data[x + y * 16] = 0x800000ff;
    }
  }
  Options opt;
  opt.max_tex = 8;
  opt.full_color = true;
  c3l::Texture desc{};
  TexUse cutout_use;
  cutout_use.alpha_opacity = true;
  const auto bytes = convert_texture(tex, opt, cutout_use, &desc, nullptr, nullptr);
  const auto image = ctr_tex::decode_level(bytes.data(), desc.w, desc.h, ctr_tex::Format::RGBA8);
  check(image.at(0, 0)[0] == 255 && image.at(0, 0)[3] == 64,
        "converter must normalize GS alpha and use the alpha-aware resize");

  // (AI-assisted) The same alpha values are material masks on opaque metal. Its visible
  // transparent-black RGB must contribute to both the resize and mip filtering.
  const auto mask_bytes = convert_texture(tex, opt, {}, &desc, nullptr, nullptr);
  const auto material = ctr_tex::decode_level(mask_bytes.data(), desc.w, desc.h,
                                             ctr_tex::Format::RGBA8);
  check(material.at(0, 0)[0] == 64 && material.at(0, 0)[3] == 64,
        "material-mask alpha must not weight visible RGB during resize");
  opt.max_tex = 16;
  const auto metal_bytes = convert_texture(tex, opt, {}, &desc, nullptr, nullptr);
  const auto metal = ctr_tex::decode_level(metal_bytes.data(), desc.w, desc.h,
                                          ctr_tex::Format::RGBA8);
  check(metal.at(1, 0)[0] == 0 && metal.at(0, 0)[0] == 255,
        "material-mask alpha must not trigger transparent RGB cleanup");
  const auto mip = ctr_tex::decode_level(metal_bytes.data() + 16 * 16 * 4, 8, 8,
                                        ctr_tex::Format::RGBA8);
  check(mip.at(0, 0)[0] == 64, "material-mask mip RGB must use ordinary box filtering");

  Converter cv;
  cv.tex_use.resize(1);
  DrawMode mode;
  mode.as_int() = 0;
  mode.set_tcc(true);
  check(mode.get_tcc_enable(), "TCC must read its own bit with texture filtering disabled");
  mode.set_tcc(false);
  mode.set_filt_enable(true);
  check(!mode.get_tcc_enable(), "texture filtering must not enable texture alpha");
  mode.set_filt_enable(false);
  mode.set_tcc(true);
  mode.set_at(true);
  mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  mode.set_aref(0x26);
  cv.note_use(0, mode.as_int(), false);
  check(cv.tex_use[0].alpha_is_opacity(), "cutout uses should retain alpha-aware filtering");
  mode.set_at(false);
  cv.note_use(0, mode.as_int(), false);
  check(!cv.tex_use[0].alpha_is_opacity(), "an opaque use must preserve low-alpha RGB in mixed uses");
}

void test_vista_seams() {
  SrcVertex v{};
  v.x = 123;
  v.y = 456;
  v.z = 789;
  std::map<std::array<u32, 7>, int> welded;
  welded.emplace(vista_vertex_key(v), 0);
  auto other = v;
  other.s = 1.f;
  welded.emplace(vista_vertex_key(other), 1);
  other = v;
  other.t = 1.f;
  welded.emplace(vista_vertex_key(other), 2);
  other = v;
  other.rgba[0] = 100;
  welded.emplace(vista_vertex_key(other), 3);
  other = v;
  other.tod = 42;
  welded.emplace(vista_vertex_key(other), 4);
  welded.emplace(vista_vertex_key(v), 5);
  check(welded.size() == 5, "vista welding must preserve UV, color and time-of-day seams");
  v.x = 0.f;
  other = v;
  other.x = -0.f;
  check(vista_vertex_key(v) == vista_vertex_key(other), "signed zero must not create a seam");
}

void test_atlas_order(bool blended) {
  Converter cv;
  Options opt;
  opt.atlas_kb_per_bind = 100;
  std::vector<c3l::Texture> descs(3);
  std::vector<std::vector<u8>> texels(3);
  cv.tex_use.resize(3);
  DrawMode mode;
  mode.as_int() = 0;
  mode.set_depth_write_enable(true);
  mode.set_zt(true);
  mode.set_depth_test(GsTest::ZTest::GEQUAL);
  mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  mode.set_ab(blended);
  for (u32 i = 0; i < 3; i++) {
    ctr_tex::Image img{8, 8, std::vector<u8>(8 * 8 * 4, 255)};
    if (i == 1) {
      for (size_t a = 3; a < img.rgba.size(); a += 4) {
        img.rgba[a] = 68;
      }
    }
    descs[i].w = descs[i].h = 8;
    descs[i].format = (u8)ctr_tex::Format::RGBA4;
    descs[i].levels = 1;
    ctr_tex::encode_level(img, ctr_tex::Format::RGBA4, 0, &texels[i]);
    const u32 key = cv.key_id(DrawKey{mode.as_int(), (u16)i, 0});
    SrcTri t{};
    t.draw_key = key;
    for (u32 k = 0; k < 3; k++) {
      SrcVertex v{};
      v.x = (float)i * 10000 + (k == 1 ? 1000.f : 0.f);
      v.y = k == 2 ? 1000.f : 0.f;
      v.s = k == 1 ? 1.f : 0.f;
      v.t = k == 2 ? 1.f : 0.f;
      std::fill(v.rgba, v.rgba + 4, 128);
      t.v[k] = (u32)cv.verts.size();
      cv.verts.push_back(v);
    }
    cv.tris.push_back(t);
  }
  DrawOptStats stats;
  optimize_draws(cv, opt, descs, texels, cv.tex_use, &stats);
  if (blended) {
    check(stats.keys_blend == 2, "fixture must drop only the fully opaque draws' blending");
    check(stats.atlases == 0, "formerly blended draws must not merge across translucent draws");
    check(cv.rank_of(cv.tris[0].draw_key) < cv.rank_of(cv.tris[1].draw_key) &&
              cv.rank_of(cv.tris[1].draw_key) < cv.rank_of(cv.tris[2].draw_key),
          "A, translucent B, C must retain their original order");
  } else {
    check(stats.atlases > 0, "ordinary opaque draws should still be atlased");
  }
}

void test_merc_lod(int feature) {
  constexpr int side = 9, center = 4 + 4 * side;
  std::vector<c3l::MercVertex> verts(side * side);
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      auto& v = verts[x + y * side];
      v.pos[0] = (s16)(x * 1000);
      v.pos[1] = (s16)(y * 1000);
      v.weights[0] = 255;
      v.normal[2] = 127;
      std::fill(v.rgba, v.rgba + 4, 128);
    }
  }
  if (feature == 0) {
    verts[center].st[0] = 16000;
  } else if (feature == 1) {
    verts[center].bones[0] = 1;
  } else {
    verts[center].normal[0] = 127;
    verts[center].normal[2] = 0;
  }
  MercOut out;
  for (int y = 0; y + 1 < side; y++) {
    for (int x = 0; x + 1 < side; x++) {
      const u16 a = (u16)(x + y * side), b = a + 1, c = a + side, d = c + 1;
      out.indices.insert(out.indices.end(), {a, b, c, b, d, c});
    }
  }
  c3l::MercDraw draw{};
  draw.eye_id = 0xff;
  draw.palette_count = 2;
  draw.palette[1] = 1;
  draw.index_count = (u32)out.indices.size();
  out.draws.push_back(draw);
  c3l::MercModel model{};
  strcpy(model.name, "regression-lod0");
  model.vertex_count = (u32)verts.size();
  model.draw_count = 1;
  model.scale = 1.f;
  Options opt;
  opt.gen_lod_error = 0.0001f;
  c3l::MercModel gen{};
  check(gen_merc_lod(model, verts, opt, &out, &gen), "simple planar model should still get a LOD");
  bool kept = false;
  for (u32 i = gen.first_draw; i < gen.first_draw + gen.draw_count; i++) {
    const auto& d = out.draws[i];
    for (u32 k = d.first_index; k < d.first_index + d.index_count; k++) {
      kept |= out.indices[k] == center;
    }
  }
  check(kept, "LOD must retain a flat surface's UV, skin-weight or normal feature");
}

// c3l v23: an RGBA8 texture's alpha mask has the same alpha in every texel of every level
void test_alpha_mask(bool four_bit) {
  ctr_tex::Image img{64, 32, std::vector<u8>(64 * 32 * 4, 0)};
  for (int y = 0; y < img.h; y++) {
    for (int x = 0; x < img.w; x++) {
      u8* p = img.at(x, y);
      p[0] = (u8)(x * 4);
      p[1] = (u8)(y * 8);
      p[2] = (u8)((x * y) & 0xff);
      p[3] = four_bit ? (u8)(17 * ((x + y) % 16)) : (u8)(40 + (x * 3 + y * 5) % 90);
    }
  }
  ctr_tex::MipOptions mo;
  mo.alpha_weighted = false;
  const auto mips = ctr_tex::make_mips(img, 3, mo);
  std::vector<u8> data;
  c3l::Texture d{};
  d.w = 64;
  d.h = 32;
  d.levels = (u8)mips.size();
  d.format = c3l::TEX_RGBA8;
  for (const auto& m : mips) {
    ctr_tex::encode_level(m, ctr_tex::Format::RGBA8, 1, &data);
  }
  Options opt;
  MaskSplit s;
  split_mask_texture(d, data, opt, true, false, &s);
  check(s.mask_desc.format == (four_bit ? c3l::TEX_A4 : c3l::TEX_A8),
        "a mask is A4 exactly when every alpha is n * 17");
  check(s.color_desc.format == c3l::TEX_ETC1 || s.color_desc.format == c3l::TEX_RGB565,
        "a mask texture's color is ETC1 or RGB565");
  u32 off = 0, moff = 0;
  for (int l = 0; l < d.levels; l++) {
    const int w = d.w >> l, h = d.h >> l;
    const auto ref = ctr_tex::decode_level(data.data() + off, w, h, ctr_tex::Format::RGBA8);
    const auto m =
        ctr_tex::decode_level(s.mask.data() + moff, w, h, (ctr_tex::Format)s.mask_desc.format);
    for (int i = 0; i < w * h; i++) {
      check(m.rgba[4 * i + 3] == ref.rgba[4 * i + 3], "a mask texel must keep the RGBA8 alpha");
    }
    off += c3l::texture_level_bytes(w, h, c3l::TEX_RGBA8);
    moff += c3l::texture_level_bytes(w, h, s.mask_desc.format);
  }
  check(moff == s.mask.size() && s.color.size() < data.size(), "mask and color sizes");
}

// RGBA5551: the PICA layout (r in the high bits, alpha in bit 0) and the 99.5% similarity gate
void test_rgba5551() {
  ctr_tex::Image px{8, 8, std::vector<u8>(8 * 8 * 4, 0)};
  px.at(0, 7)[0] = 255;  // bottom row first: texel 0
  px.at(0, 7)[3] = 255;
  std::vector<u8> one;
  ctr_tex::encode_level(px, ctr_tex::Format::RGBA5551, 0, &one);
  check(one.size() == 128 && one[0] == 0x01 && one[1] == 0xf8, "RGBA5551 is r5 g5 b5 a1");

  // colors a 5-bit channel holds, alpha 0 / 255 in every level: kept exactly
  auto make = [](bool exact_color, bool soft_alpha, int cell = 8, u8 opaque = 255) {
    ctr_tex::Image img{32, 32, std::vector<u8>(32 * 32 * 4, 0)};
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 32; x++) {
        u8* p = img.at(x, y);
        for (int c = 0; c < 3; c++) {
          const int q = (x * 3 + y * 7 + c * 11) % 32;
          p[c] = exact_color ? (u8)((q << 3) | (q >> 2)) : (u8)((x * 37 + y * 11 + c * 53) % 256);
        }
        p[3] = (x / cell + y / cell) % 2 ? opaque : (soft_alpha ? (u8)(20 + x * 2) : 0);
      }
    }
    return img;
  };
  auto rgba8 = [](const ctr_tex::Image& img, int levels, c3l::Texture* d) {
    std::vector<u8> data;
    ctr_tex::MipOptions mo;
    for (const auto& m : ctr_tex::make_mips(img, levels, mo)) {
      ctr_tex::encode_level(m, ctr_tex::Format::RGBA8, 1, &data);
    }
    *d = c3l::Texture{};
    d->w = d->h = 32;
    d->levels = (u8)levels;
    d->format = c3l::TEX_RGBA8;
    return data;
  };
  Options opt;
  c3l::Texture d;
  std::vector<u8> out;
  Rgba5551Gate g;
  auto data = rgba8(make(true, false), 1, &d);
  check(to_rgba5551(d, data, opt, true, false, 76, &out, &g) && g.alpha_exact &&
            g.similarity == 1.0 && out.size() == 32 * 32 * 2,
        "5-bit colors with 0 / 1 alpha pass exactly");
  const auto ref = ctr_tex::decode_level(data.data(), 32, 32, ctr_tex::Format::RGBA8);
  check(ctr_tex::decode_level(out.data(), 32, 32, ctr_tex::Format::RGBA5551).rgba == ref.rgba,
        "an exact RGBA5551 texture decodes to the RGBA8 one");

  data = rgba8(make(false, false), 1, &d);
  to_rgba5551(d, data, opt, true, false, 76, &out, &g);
  check(g.similarity < 1.0 && g.color_ok == (g.similarity >= 0.99),
        "8-bit colors pass only at 99% color similarity or more");

  data = rgba8(make(true, false, 1), 3, &d);
  check(!to_rgba5551(d, data, opt, true, false, 76, &out, &g) && !g.alpha_exact,
        "mips with blended alpha need the alpha test rule");

  data = rgba8(make(true, true), 1, &d);
  check(!to_rgba5551(d, data, opt, true, false, 76, &out, &g) && !g.alpha_ok,
        "soft alpha without the alpha test fails");
  check(!to_rgba5551(d, data, opt, true, true, 76, &out, &g) && g.coverage < 0.995,
        "soft alpha whose filtered edge moves fails");
  // alpha 0 / 254 (GS 0x7f), alpha tested: the filtered samples pass or fail as before
  data = rgba8(make(true, false, 8, 254), 3, &d);
  check(!to_rgba5551(d, data, opt, true, false, 76, &out, &g) && !g.alpha_exact,
        "alpha 254 is not 1 bit");
  check(to_rgba5551(d, data, opt, true, true, 76, &out, &g) && g.coverage == 1.0,
        "alpha tested: every sample passes or fails as before");
  u32 off = 0, off5 = 0;
  for (int l = 0; l < 3; l++) {
    const int w = 32 >> l;
    const auto dec = ctr_tex::decode_level(out.data() + off5, w, w, ctr_tex::Format::RGBA5551);
    const auto src = ctr_tex::decode_level(data.data() + off, w, w, ctr_tex::Format::RGBA8);
    for (int i = 0; i < w * w; i++) {
      check((dec.rgba[4 * i + 3] >= 76) == (src.rgba[4 * i + 3] >= 76),
            "the alpha bit keeps each texel's alpha test result in every level");
    }
    off += c3l::texture_level_bytes(w, w, c3l::TEX_RGBA8);
    off5 += c3l::texture_level_bytes(w, w, c3l::TEX_RGBA5551);
  }
  check(off5 == out.size(), "RGBA5551 levels: 2 bytes per texel");
}
}  // namespace

// the tfrag geomorph's versions (c3l::ExtTfrag): a version the same as the one before it left out
void test_tfrag_versions() {
  float d[3];
  tfrag_lod::subdivide_dists(0, 0, d);
  check(std::abs(d[0] / 4096.f - 38.5714f) < 0.01f && d[1] == 50.f * 4096.f &&
            d[2] == 70.f * 4096.f,
        "the default subdivide distances are 38.6 / 50 / 70 m");
  Converter cv;
  for (int i = 0; i < 4; i++) {
    SrcVertex v;
    v.x = (float)(i & 1);
    v.y = (float)(i >> 1);
    v.z = 0;
    cv.verts.push_back(v);
  }
  auto tri = [&](u32 g, u8 lod, u32 c) {
    SrcTri t{{0, 1, c}, 0};
    t.tgroup = g;
    t.tlod = lod;
    cv.tris.push_back(t);
  };
  // group 0: three versions the same; 1: mid = far; 2: near = mid; 3: all different
  for (u32 g = 0; g < 4; g++) {
    cv.tgroup_of[g] = g;
  }
  tri(0, c3l::kRunLodNear, 2), tri(0, c3l::kRunLodMid, 2), tri(0, c3l::kRunLodFar, 2);
  tri(1, c3l::kRunLodNear, 2), tri(1, c3l::kRunLodMid, 3), tri(1, c3l::kRunLodFar, 3);
  tri(2, c3l::kRunLodNear, 2), tri(2, c3l::kRunLodMid, 2), tri(2, c3l::kRunLodFar, 3);
  tri(3, c3l::kRunLodNear, 2), tri(3, c3l::kRunLodMid, 3), tri(3, c3l::kRunLodFar, 1);
  cv.tfrag_drop_same_versions(0, 50.f, 70.f);
  std::map<u32, std::vector<u8>> lods;
  for (const auto& t : cv.tris) {
    lods[t.tgroup].push_back(t.tlod);
  }
  check(lods[0] == std::vector<u8>{c3l::kRunLodAlways}, "one geometry: always drawn");
  check(lods[1].size() == 2 && cv.tgroup_far[1] == std::numeric_limits<float>::max(),
        "mid = far: no far version");
  check(lods[2].size() == 2 && cv.tgroup_mid[2] == 70.f, "near = mid: near until the far one");
  check(lods[3].size() == 3 && cv.tgroup_mid[3] == 50.f && cv.tgroup_far[3] == 70.f,
        "three versions");
}

// (AI-assisted, terraincull) ground at 0 m with a 10 m deep pit at x 60..80 m: a floor triangle
// far from the pit is marked (and turned to face up), one by the pit's edge, one floating 10 m up
// and a wall are not
void test_terrain_cull() {
  constexpr float m = 4096.f;
  tfrag3::Level level;
  auto quad = [&](float x0, float x1, float z0, float z1, float y) {
    const float p[4][2] = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
    for (const auto& f : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
      for (int k : f) {
        tfrag3::CollisionMesh::Vertex v{};
        v.x = p[k][0] * m;
        v.y = y * m;
        v.z = p[k][1] * m;
        level.collision.vertices.push_back(v);
      }
    }
  };
  quad(-100, 60, -100, 100, 0);
  quad(60, 80, -100, 100, -10);
  quad(80, 200, -100, 100, 0);
  Converter cv;
  cv.level = &level;
  DrawMode mode;
  mode.as_int() = 0;
  mode.set_depth_write_enable(true);
  mode.set_zt(true);
  mode.set_depth_test(GsTest::ZTest::GEQUAL);
  DrawKey k{};
  k.mode = mode.as_int();
  k.texture = 0xffff;
  k.kind = 0;  // tfrag
  const u32 key = cv.key_id(k);
  auto add = [&](std::array<std::array<float, 3>, 3> p) {
    SrcTri t{};
    for (int c = 0; c < 3; c++) {
      SrcVertex v{};
      v.x = p[c][0] * m;
      v.y = p[c][1] * m;
      v.z = p[c][2] * m;
      t.v[c] = (u32)cv.verts.size();
      cv.verts.push_back(v);
    }
    t.draw_key = key;
    t.tier = 1;
    cv.tris.push_back(t);
    return cv.tris.size() - 1;
  };
  // wound to face down (the converter turns it up)
  const size_t floor = add({{{0, 0, 0}, {0, 0, 4}, {4, 0, 0}}});
  const size_t edge = add({{{50, 0, 0}, {54, 0, 0}, {50, 0, 4}}});
  const size_t high = add({{{-50, 10, 0}, {-46, 10, 0}, {-50, 10, 4}}});
  const size_t wall = add({{{10, 0, 0}, {14, 0, 0}, {10, 4, 0}}});
  cv.mark_terrain_cull();
  auto marked = [&](size_t i) { return (cv.draw_keys[cv.tris[i].draw_key].kind & c3l::kDrawCullTerrain) != 0; };
  check(marked(floor), "open ground far from any drop is marked");
  {
    const auto& t = cv.tris[floor];
    const auto& a = cv.verts[t.v[0]];
    const auto& b = cv.verts[t.v[1]];
    const auto& c = cv.verts[t.v[2]];
    const double ny = (double)(b.z - a.z) * (c.x - a.x) - (double)(b.x - a.x) * (c.z - a.z);
    check(ny > 0, "a marked triangle faces up");
  }
  check(!marked(edge), "ground within 20 m of a 10 m drop is not");
  check(!marked(high), "a floor over no walkable ground of its own is not");
  check(!marked(wall), "a wall is not");
}

int main() {
  try {
    lg::initialize();
    run_cpu_bench_tests();
    test_terrain_cull();
    test_resize();
    test_vista_seams();
    test_atlas_order(true);
    test_atlas_order(false);
    for (int feature = 0; feature < 3; feature++) {
      test_merc_lod(feature);
    }
    test_alpha_mask(false);
    test_alpha_mask(true);
    test_rgba5551();
    test_tfrag_versions();
    run_vram_tests();
    run_citadel_tests();
    fmt::print("3DS rendering regressions passed (CPU only).\n");
    return 0;
  } catch (const std::exception& e) {
    fmt::print(stderr, "3DS regression failed: {}\n", e.what());
    return 1;
  }
}
