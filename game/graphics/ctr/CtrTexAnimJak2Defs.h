#pragma once

/*!
 * @file CtrTexAnimJak2Defs.h
 * (AI-assisted)
 * Jak II's texture animations for the 3DS (CtrTexAnimJak2): the definitions of the PC port's
 * TextureAnimator (TextureAnimatorDefs.cpp setup_texture_anims_common / _jak2) as plain tables,
 * shared by the runtime and the level converter (ctr_level_converter --texanim, which writes the
 * source textures they name into texanim.c3t, see CtrTexAnimJak2.h).
 *
 * Fixed animations: each destination texture is cleared to black (alpha 0x80), then each layer
 * active at the animation's time draws its source texture as a quad whose corners, texture
 * coordinates and color the game sends (LayerVals, interpolated over the layer's time range),
 * blended Cs * As + Cd (kAdd: set_blend_b2_d1) or Cs * As + Cd * (1 - As) (kMix: set_blend_b1_d1).
 * CLUT blends: index textures whose colors are a blend of two CLUTs (the game sends the weight).
 */

#include <cstdint>

namespace ctr_texanim_jak2 {

enum : uint8_t { kAdd = 0, kMix = 1 };

struct Layer {
  const char* tex;  // source texture (in GAME.fr3)
  float start, end;
  uint8_t blend;  // kAdd / kMix
  bool clamp;     // clamp to edge (else repeat)
};

struct Anim {
  const char* dest;  // the animated slot (jak2_animated_texture_slots) and its base texture
  uint16_t w, h;     // override_size (0: the base texture's size)
  bool to_vram;      // move_to_pool: also replaces the texture at the game's address (tbp)
  uint8_t layers;
  Layer layer[5];
};

struct Array {
  uint16_t code;  // PcTextureAnimCodesJak2
  uint8_t anims;
  Anim anim[4];
};

// clang-format off
constexpr Array kArrays[] = {
  // SKULL_GEM
  {27, 1, {{"skull-gem-dest", 32, 32, true, 3,
            {{"skull-gem-alpha-00", 0, 300, kAdd, false},
             {"skull-gem-alpha-01", 0, 300, kAdd, false},
             {"skull-gem-alpha-02", 0, 300, kAdd, false}}}}},
  // BOMB
  {28, 1, {{"bomb-gradient", 0, 0, false, 2,
            {{"bomb-gradient-rim", 0, 300, kAdd, false},
             {"bomb-gradient-flames", 0, 300, kAdd, false}}}}},
  // CAS_CONVEYOR
  {29, 4, {{"cas-conveyor-dest", 64, 32, false, 1, {{"cas-conveyor", 0, 300, kAdd, false}}},
           {"cas-conveyor-dest-01", 64, 32, false, 1, {{"cas-conveyor", 0, 300, kAdd, false}}},
           {"cas-conveyor-dest-02", 64, 32, false, 1, {{"cas-conveyor", 0, 300, kAdd, false}}},
           {"cas-conveyor-dest-03", 64, 32, false, 1, {{"cas-conveyor", 0, 300, kAdd, false}}}}},
  // SECURITY
  {30, 2, {{"security-env-dest", 0, 0, false, 2,
            {{"security-env-uscroll", 0, 4800, kAdd, false},
             {"security-env-uscroll", 0, 4800, kAdd, false}}},
           {"security-dot-dest", 0, 0, false, 3,
            {{"common-white", 0, 4800, kAdd, false},
             {"security-dot-src", 0, 600, kAdd, false},
             {"security-dot-src", 0, 600, kAdd, false}}}}},
  // WATERFALL, WATERFALL_B
  {31, 1, {{"waterfall-dest", 0, 0, false, 4,
            {{"waterfall", 0, 450, kMix, false}, {"waterfall", 0, 450, kMix, false},
             {"waterfall", 0, 450, kMix, false}, {"waterfall", 0, 450, kMix, false}}}}},
  {32, 1, {{"waterfall-dest", 0, 0, false, 4,
            {{"waterfall", 0, 450, kMix, false}, {"waterfall", 0, 450, kMix, false},
             {"waterfall", 0, 450, kMix, false}, {"waterfall", 0, 450, kMix, false}}}}},
  // LAVA, LAVA_B
  {33, 1, {{"dig-lava-01-dest", 0, 0, false, 2,
            {{"dig-lava-01", 0, 3600, kMix, false}, {"dig-lava-01", 0, 3600, kMix, false}}}}},
  {34, 1, {{"dig-lava-01-dest", 0, 0, false, 2,
            {{"dig-lava-01", 0, 3600, kMix, false}, {"dig-lava-01", 0, 3600, kMix, false}}}}},
  // STADIUMB
  {35, 1, {{"stdmb-energy-wall-01-dest", 0, 0, false, 2,
            {{"stdmb-energy-wall-01", 0, 300, kMix, false},
             {"stdmb-energy-wall-01", 0, 300, kMix, false}}}}},
  // FORTRESS_PRIS
  {36, 2, {{"robotank-tread-l-dest", 0, 0, false, 1, {{"robotank-tread", 0, 1, kMix, false}}},
           {"robotank-tread-r-dest", 0, 0, false, 1, {{"robotank-tread", 0, 1, kMix, false}}}}},
  // FORTRESS_WARP
  {37, 1, {{"fort-roboscreen-dest", 0, 0, true, 1,
            {{"fort-roboscreen-env", 0, 300, kAdd, false}}}}},
  // METKOR
  {38, 1, {{"squid-env-rim-dest", 0, 0, true, 5,
            {{"metkor-head-env-noise", 0, 1200, kAdd, true},
             {"metkor-head-env-scan", 0, 1200, kAdd, true},
             {"metkor-head-env-rim", 0, 1200, kAdd, true},
             {"metkor-head-env-rim", 0, 1200, kAdd, true},
             {"environment-phong-rim", 0, 1200, kAdd, true}}}}},
  // SHIELD
  {39, 1, {{"squid-env-rim-dest", 0, 0, true, 5,
            {{"common-white", 0, 1200, kAdd, true},
             {"squid-env-uscroll", 0, 1200, kAdd, true},
             {"squid-env-uscroll", 0, 1200, kAdd, true},
             {"squid-env-rim-src", 0, 1200, kAdd, true},
             {"squid-env-rim-src", 0, 1200, kAdd, true}}}}},
  // KREW_HOLO
  {40, 1, {{"krew-holo-dest", 0, 0, true, 5,
            {{"metkor-head-env-noise", 0, 1200, kAdd, true},
             {"metkor-head-env-scan", 0, 1200, kAdd, true},
             {"metkor-head-env-rim", 0, 1200, kAdd, true},
             {"metkor-head-env-rim", 0, 1200, kAdd, true},
             {"metkor-phong-env", 0, 1200, kAdd, true}}}}},
};
// clang-format on

/*! A texture of a CLUT blend group: the index texture `name` with the CLUTs of name + suf0 and
 * name + suf1, from the index textures of level `dgo` (nullptr: any). */
struct ClutTex {
  const char* name;
  const char* suf0;
  const char* suf1;
  const char* dgo;
};
struct ClutGroup {
  uint16_t code;  // PcTextureAnimCodesJak2
  bool to_vram;   // move_to_pool
  uint8_t count;
  ClutTex tex[8];
};

// clang-format off
constexpr ClutGroup kClutGroups[] = {
  // DARKJAK
  {22, false, 4, {{"jakbsmall-eyebrow", "-norm", "-dark", nullptr},
                  {"jakbsmall-face", "-norm", "-dark", nullptr},
                  {"jakbsmall-finger", "-norm", "-dark", nullptr},
                  {"jakbsmall-hair", "-norm", "-dark", nullptr}}},
  // PRISON_JAK
  {23, false, 6, {{"jak-orig-arm-formorph", "-start", "-end", "LDJAKBRN.DGO"},
                  {"jak-orig-eyebrow-formorph", "-start", "-end", "LDJAKBRN.DGO"},
                  {"jak-orig-finger-formorph", "-start", "-end", "LDJAKBRN.DGO"},
                  {"jakb-facelft", "-norm", "-dark", "LDJAKBRN.DGO"},
                  {"jakb-facert", "-norm", "-dark", "LDJAKBRN.DGO"},
                  {"jakb-hairtrans", "-norm", "-dark", "LDJAKBRN.DGO"}}},
  // ORACLE_JAK
  {24, true, 5, {{"jakb-eyebrow", "-norm", "-dark", "ORACLE.DGO"},
                 {"jakb-eyelid", "-norm", "-dark", "ORACLE.DGO"},
                 {"jakb-facelft", "-norm", "-dark", "ORACLE.DGO"},
                 {"jakb-facert", "-norm", "-dark", "ORACLE.DGO"},
                 {"jakb-hairtrans", "-norm", "-dark", "ORACLE.DGO"}}},
  // NEST_JAK
  {25, true, 5, {{"jakb-eyebrow", "-norm", "-dark", "NEB.DGO"},
                 {"jakb-eyelid", "-norm", "-dark", "NEB.DGO"},
                 {"jakb-facelft", "-norm", "-dark", "NEB.DGO"},
                 {"jakb-facert", "-norm", "-dark", "NEB.DGO"},
                 {"jakb-hairtrans", "-norm", "-dark", "NEB.DGO"}}},
};
// clang-format on

// the slime (SLIME: TextureAnimator::run_slime): its two slots
constexpr const char* kSlimeDest = "cas-toxic-slime-dest";
constexpr const char* kSlimeScrollDest = "cas-toxic-slime-scroll-dest";

}  // namespace ctr_texanim_jak2

/*!
 * (AI-assisted) texanim.c3t, next to the .c3l files (ctr_level_converter --texanim GAME.fr3 FILE):
 * the textures the tables above name, from GAME.fr3 (the PC's common level, which holds every
 * animated texture's sources). Header, Entry[count], then the data. Sizes at most kMaxSize per side
 * (larger ones halved: RGBA box filtered, index textures every other texel).
 */
namespace c3t {
constexpr char kMagic[4] = {'C', '3', 'T', 'A'};
constexpr uint32_t kVersion = 1;
constexpr int kMaxSize = 64;
enum : uint8_t {
  kRgba = 0,      // w * h RGBA8 (alpha 0x80 = 1.0), rows from the top
  kIndex = 1,     // w * h indices, then 256 RGBA8 colors (the color table, by index)
  kSizeOnly = 2,  // a destination: only its size
};
struct Header {
  char magic[4];
  uint32_t version;
  uint32_t count;
  uint32_t entries_offset;
};
static_assert(sizeof(Header) == 16);
struct Entry {
  char name[40];
  char dgo[16];  // index textures of a CLUT group with a level: its DGO, else ""
  uint16_t w, h;
  uint8_t kind;
  uint8_t pad[3];
  uint32_t offset, size;
};
static_assert(sizeof(Entry) == 72);
}  // namespace c3t
