// (AI-assisted) Real VRAM/cache code; GPU allocation is a deterministic failing/succeeding stub.
#include "game/graphics/ctr/CtrVram.cpp"
#include "check.h"

namespace {
int allocations = 0;
bool fail_next = false;
int allocate_texture() {
  allocations++;
  if (fail_next) {
    fail_next = false;
    return -1;
  }
  return allocations;
}
constexpr u64 kTex0 = (1ull << 14) | (3ull << 26) | (3ull << 30);
}  // namespace

extern "C" {
int ctr_gpu_asm_tex() { return 0; }
int ctr_gpu_tex_create(int, int, const uint8_t*) { return allocate_texture(); }
int ctr_gpu_tex_create_compact(int, int, const uint8_t*, int) { return allocate_texture(); }
void ctr_gpu_tex_delete(int) {}
void ctr_gpu_tex_update(int, const uint8_t*) {}
double ctr_gpu_time_ms() { return 0; }
}
namespace ctr_button_art {
u32 generation() { return 0; }
bool may_patch(u64) { return false; }
bool patch(u64, u32*, int, int) { return false; }
}  // namespace ctr_button_art

void run_vram_tests() {
  for (bool sparse : {false, true}) {
    CtrVram vram(sparse);
    std::vector<u32> pixels(64 * 32, 0x800000ff);
    vram.upload_ct32((const u8*)pixels.data(), 0, 64, 32);
    const int before = allocations;
    fail_next = true;
    check(vram.get_texture(kTex0) == nullptr, "failed allocations must not return cached handles");
    const auto* texture = vram.get_texture(kTex0);
    check(texture && texture->handle >= 0 && allocations == before + 2,
          "allocation must retry without a VRAM change");
    check(vram.get_texture(kTex0) == texture && allocations == before + 2,
          "successful allocations must still be cached");
    pixels[0] ^= 0xff;
    vram.upload_ct32((const u8*)pixels.data(), 0, 64, 32);
    fail_next = true;
    check(vram.get_texture(kTex0) == nullptr, "failed replacement must not publish a new signature");
    texture = vram.get_texture(kTex0);
    check(texture && texture->handle >= 0 && allocations == before + 4,
          "failed replacement must retry even with an older cached entry");

    // another page in the same EE allocation must be written; the same page again must not
    std::vector<u8> ee(4096 + 16384, 0);
    GoalTexturePageHeader page{};
    page.id = 1;
    page.size = 4096;
    page.segment[0].block_data_ptr = 4096;
    page.segment[0].size = 4096;
    auto upload_and_decode = [&]() {
      vram.upload_texture_page_now((const u8*)&page, -1, ee.data(), 0);
      std::vector<u32> decoded;
      int w, h;
      check(vram.decode(kTex0, &decoded, &w, &h), "page fixture must decode");
      return decoded;
    };
    upload_and_decode();
    const u32 written = vram.stats().chunks_written;
    upload_and_decode();
    check(vram.stats().chunks_written == (int)written,
          "unchanged page uploads must still be deduplicated");
    const u64 old_id = page_chunk_id(ee.data() + 4096, 1);
    check(page_chunk_id(ee.data() + 4096, 2) != old_id, "chunk ids must depend on the page");
    const u32 color = 0x8000007f;
    memcpy(ee.data() + 4096 + 4, &color, 4);
    page.id = 2;
    const auto decoded = upload_and_decode();
    check(vram.stats().chunks_written == (int)written + 1 && decoded[1] == color,
          "another page in the same allocation must be written");
    upload_and_decode();
    check(vram.stats().chunks_written == (int)written + 1,
          "the new page uploaded again must be deduplicated");
  }

  // a boot "effects" page: its texture decoded ahead (prewarm) is the one its first draw gets
  {
    std::vector<u8> ee(65536, 0);
    const char name[] = "effects";
    memcpy(ee.data() + 64 + 4, name, sizeof(name));
    GoalTexture gt{};
    gt.w = gt.h = 16;
    gt.psm = 0;
    gt.dest[0] = 64;
    gt.width[0] = 1;
    memcpy(ee.data() + 128, &gt, sizeof(gt));
    for (u32 k = 0; k < 4096; k++) {
      const u32 v = 0x40000000u | (k * 2654435761u >> 8);
      memcpy(ee.data() + 4096 + 4 * k, &v, 4);
    }
    struct {
      GoalTexturePageHeader h;
      u32 ptrs[1];
    } tp{};
    tp.h.name_ptr = 64;
    tp.h.length = 1;
    tp.h.size = 4096;
    tp.h.segment[0].block_data_ptr = 4096;
    tp.h.segment[0].size = 4096;
    tp.h.segment[0].dest = 64 * 64;
    tp.ptrs[0] = 128;
    // the game's TEX0 (adgif-shader<-texture!), with a texture function of its own
    const u64 game_tex0 = 64 | 1ull << 14 | 4ull << 26 | 4ull << 30 | 1ull << 34 | 1ull << 35 |
                          1ull << 61;
    CtrVram ref(true, true), vram(true, true);  // Jak 1: sparse, boot pages prewarmed
    ref.upload_texture_page_now((const u8*)&tp, -1, ee.data(), 0);
    vram.upload_texture_page_now((const u8*)&tp, -1, ee.data(), 0);
    for (int f = 0; f < 40; f++) {
      vram.prewarm(1.0);
    }
    check(vram.stats().decoded == 1, "the page's texture must be decoded ahead");
    const auto* a = ref.get_texture(game_tex0);
    const auto* b = vram.get_texture(game_tex0);
    check(a && b && vram.stats().decoded == 1 && vram.stats().cached == 1,
          "the first draw must find the texture decoded ahead");
    check(a->w == b->w && a->h == b->h && a->max_alpha == b->max_alpha &&
              memcmp(a->box, b->box, sizeof(a->box)) == 0,
          "the texture decoded ahead must be the first draw's");
  }

  // two pages alternating in the same VRAM, each read by its texture: the textures stay cached
  // and the chunks' bytes are written only when something decodes
  for (bool sparse : {false, true}) {
    CtrVram vram(sparse);
    std::vector<u8> ee(4096 + 2 * 16384, 0);
    GoalTexturePageHeader pages[2]{};
    for (int i = 0; i < 2; i++) {
      pages[i].id = 10 + i;
      pages[i].size = 4096;
      pages[i].segment[0].block_data_ptr = 4096 + 16384 * i;
      pages[i].segment[0].size = 4096;
      for (u32 k = 0; k < 4096; k++) {
        const u32 v = 0x80000000u | (k * 2654435761u >> 8) ^ (i ? 0x5a5a5au : 0);
        memcpy(ee.data() + 4096 + 16384 * i + 4 * k, &v, 4);
      }
    }
    const u64 tex[2] = {kTex0, (1ull << 14) | (4ull << 26) | (4ull << 30)};
    auto reference = [&](int i, u64 t0) {
      CtrVram ref(sparse);
      ref.upload_texture_page_now((const u8*)&pages[i], -1, ee.data(), 0);
      std::vector<u32> out;
      int w, h;
      check(ref.decode(t0, &out, &w, &h), "reference decode");
      return out;
    };
    for (int round = 0; round < 4; round++) {
      for (int i = 0; i < 2; i++) {
        vram.upload_texture_page((const u8*)&pages[i], -1, ee.data(), 0);
        const int written = vram.stats().chunks_written;
        const auto* t = vram.get_texture(tex[i]);
        check(t && t->handle >= 0, "alternating page texture");
        if (round > 0) {
          check(vram.stats().chunks_written == written,
                "an alternating page whose textures are cached must not be written");
        }
      }
    }
    for (int i = 0; i < 2; i++) {
      vram.upload_texture_page((const u8*)&pages[i], -1, ee.data(), 0);
      check(vram.get_texture(tex[i]) != nullptr, "alternating page texture");
      std::vector<u32> out;
      int w, h;
      check(vram.decode_for_cpu(tex[i], &out, &w, &h) && out == reference(i, tex[i]),
            "a lazy chunk must decode as the page written");
      check(vram.decode_for_cpu(tex[1 - i], &out, &w, &h) && out == reference(i, tex[1 - i]),
            "the other texture must read the page written last");
    }
  }

  // every format at page aligned and unaligned addresses and past the end of VRAM: the texels the
  // address functions of texture_conversion.h give, one by one
  for (bool sparse : {false, true}) {
    CtrVram vram(sparse);
    std::vector<u32> img(512 * 2048);
    u32 seed = 12345;
    for (auto& v : img) {
      seed = seed * 1664525u + 1013904223u;
      v = seed;
    }
    vram.upload_ct32((const u8*)img.data(), 0, 512, 2048);
    std::vector<u8> flat(4 * 1024 * 1024);
    for (u32 y = 0; y < 2048; y++) {
      for (u32 x = 0; x < 512; x++) {
        memcpy(flat.data() + psmct32_addr(x, y, 512), &img[x + 512 * y], 4);
      }
    }
    auto rd32 = [&](u32 a) {
      u32 v;
      memcpy(&v, flat.data() + (a & 0x3ffffc), 4);
      return v;
    };
    auto clut_ref = [&](u32 cbp, u32 cpsm, u32 e) -> u32 {
      const u32 chunk = e / 16, off = e % 16;
      const u32 x = (chunk & 1) * 8 + off % 8, y = (chunk >> 1) * 2 + off / 8;
      if (cpsm == 0) {
        return rd32(psmct32_addr(x, y, 64) + cbp * 256);
      }
      u16 c;
      memcpy(&c, flat.data() + ((psmct16_addr(x, y, 64) + cbp * 256) & 0x3ffffe), 2);
      return ct16_to_rgba(c);
    };
    auto reference = [&](u64 tex0) {
      const GsTex0 t(tex0);
      const int w = 1 << std::min(t.tw(), 10u), h = 1 << std::min(t.th(), 10u);
      const u32 tbp = t.tbp0(), width = std::max(1u, t.tbw()) * 64, csa = (tex0 >> 56) & 0x1f;
      const u32 cbp = t.cbp(), cpsm = t.cpsm();
      std::vector<u32> px(w * h);
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          u32 v = 0;
          switch (t.psm()) {
            case GsTex0::PSM::PSMCT32:
              v = rd32(psmct32_addr(x, y, width) + tbp * 256);
              break;
            case GsTex0::PSM::PSMCT24:
              v = (rd32(psmct32_addr(x, y, width) + tbp * 256) & 0xffffff) | 0x80000000u;
              break;
            case GsTex0::PSM::PSMCT16:
            case GsTex0::PSM::PSMCT16S: {
              u16 c;
              memcpy(&c, flat.data() + ((psmct16_addr(x, y, width) + tbp * 256) & 0x3ffffe), 2);
              v = ct16_to_rgba(c);
            } break;
            case GsTex0::PSM::PSMT8:
              v = clut_ref(cbp, cpsm, flat[(psmt8_addr(x, y, width) + tbp * 256) & 0x3fffff]);
              break;
            case GsTex0::PSM::PSMT4: {
              const u32 a = (psmt4_addr_half_byte(x, y, width) + tbp * 512) & 0x7fffff;
              const u8 b = flat[a / 2];
              v = clut_ref(cbp, cpsm, csa * 16 + ((a & 1) ? (b >> 4) : (b & 0xf)));
            } break;
            case GsTex0::PSM::PSMT8H:
              v = clut_ref(cbp, cpsm, rd32(psmct32_addr(x, y, width) + tbp * 256) >> 24);
              break;
            case GsTex0::PSM::PSMT4HH:
              v = clut_ref(cbp, cpsm, csa * 16 + (rd32(psmct32_addr(x, y, width) + tbp * 256) >> 28));
              break;
            default:
              v = clut_ref(cbp, cpsm,
                           csa * 16 + ((rd32(psmct32_addr(x, y, width) + tbp * 256) >> 24) & 0xf));
              break;
          }
          px[x + y * w] = v;
        }
      }
      return px;
    };
    const u32 sizes[4][2] = {{3, 3}, {5, 4}, {7, 6}, {8, 8}};
    int k = 0;
    for (u64 psm : {0x0, 0x1, 0x2, 0xa, 0x13, 0x14, 0x1b, 0x24, 0x2c}) {
      for (u64 tbp : {0, 32, 5, 37, 16160, 16343}) {
        for (u64 tbw : {1, 2, 4, 8}) {
          for (const auto& sz : sizes) {
            k++;
            const u64 tex0 = tbp | tbw << 14 | psm << 20 | (u64)sz[0] << 26 | (u64)sz[1] << 30 |
                             (u64)(1000 + 7 * (k % 5)) << 37 | (u64)(k % 3 == 0 ? 2 : 0) << 51 |
                             (u64)(k % 4) << 56;
            std::vector<u32> out;
            int w, h;
            check(vram.decode(tex0, &out, &w, &h) && out == reference(tex0),
                  "a decoded texture must hold the texels the address functions give");
          }
        }
      }
    }
    // a raw image's texture: the image's rows, the last one repeated below them
    CtrVram raw(sparse);
    raw.upload_ct32((const u8*)img.data(), 0, 512, 416);
    std::vector<u32> out;
    int w, h;
    check(raw.decode_for_cpu(8ull << 14 | 9ull << 26 | 9ull << 30, &out, &w, &h) && w == 512 &&
              h == 512,
          "raw image texture");
    for (int y = 0; y < 512; y++) {
      check(!memcmp(&out[512 * y], &img[512 * std::min(y, 415)], 512 * 4),
            "a raw image's texture must hold its rows, the last one below them");
    }
  }
}
