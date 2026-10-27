// (AI-assisted) Exercise the converter's actual internal passes with small synthetic assets.
#define main ctr_level_converter_main
#include "tools/ctr_level_converter/main.cpp"
#undef main

#include "check.h"

void run_vram_tests();
void run_citadel_tests();

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
}  // namespace

int main() {
  try {
    lg::initialize();
    test_resize();
    test_vista_seams();
    test_atlas_order(true);
    test_atlas_order(false);
    for (int feature = 0; feature < 3; feature++) {
      test_merc_lod(feature);
    }
    run_vram_tests();
    run_citadel_tests();
    fmt::print("3DS rendering regressions passed (CPU only).\n");
    return 0;
  } catch (const std::exception& e) {
    fmt::print(stderr, "3DS regression failed: {}\n", e.what());
    return 1;
  }
}
