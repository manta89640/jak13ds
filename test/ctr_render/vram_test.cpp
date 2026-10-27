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

    // A page source and destination stay fixed while one word changes. Word 1 was missed
    // by both samplers; word 64 was noticed by the page sampler but missed by the chunk's.
    std::vector<u8> ee(4096 + 16384, 0);
    GoalTexturePageHeader page{};
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
    const u64 old_id = page_chunk_id(ee.data() + 4096);
    const u32 color = 0x8000007f;
    memcpy(ee.data() + 4096 + 4, &color, 4);
    check(page_chunk_id(ee.data() + 4096) != old_id, "chunk hash must cover formerly unsampled words");
    const auto decoded = upload_and_decode();
    check(decoded[1] == color, "page cache must notice a formerly unsampled texel edit");
    const u32 written = vram.stats().chunks_written;
    ee[4096 + 64 * 4] ^= 1;
    upload_and_decode();
    check(vram.stats().chunks_written == (int)written + 1,
          "chunk cache must not discard an edit detected by the page cache");
    upload_and_decode();
    check(vram.stats().chunks_written == (int)written + 1,
          "unchanged page uploads must still be deduplicated");
  }
}
