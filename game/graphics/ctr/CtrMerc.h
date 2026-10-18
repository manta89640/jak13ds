#pragma once

/*!
 * @file CtrMerc.h
 * (AI-assisted)
 * Merc (skinned characters and objects) for the 3DS renderer. Reads the same PC-port model
 * records as Merc2 (model name, lights, bone matrices, effect flags), finds the model in the
 * loaded .c3l files and draws it with GPU skinning (draws are pre-split by the converter so each
 * uses at most 24 bones). Not done: lighting (a per-model tint from the ambient + first light),
 * blend shapes, envmap, eyes. (AI-assisted, far-fog) Fog: the game's, per vertex (merc_fog).
 * (AI-assisted) Vertices the game rewrites in its merc data (ripple water, texture scroll) are
 * unpacked from it each frame like Merc2::model_mod_draws (apply_mod).
 */

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/math/Vector.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/ctr_gpu.h"

class CtrLevels;

// (AI-assisted) a bone matrix: the transform part of the game's (tmat, 64 of its 112 bytes; the
// normal matrix nmat[3] after it is never read: the skin shaders turn normals with tmat's rows)
struct CtrMercMat {
  math::Vector4f tmat[4];
};

class CtrMercRenderer : public CtrBucketRenderer {
 public:
  CtrMercRenderer(std::string name, int id, CtrLevels* levels);
  void render(DmaFollower& dma, CtrRenderState& rs) override;
  /*! Where the eye draws (c3l::MercDraw::eye_id) get their textures. */
  void set_eye_renderer(const class CtrEyeRenderer* eyes) { m_eyes = eyes; }

 private:
  /*! Blend shapes: move the model's face vertices for these weights (all 0: the base pose). */
  void apply_blerc(const struct CtrLevelData& lev,
                   const struct CtrMercModelData& model,
                   const float* weights);
  /*! (AI-assisted) Copy the game's vertex data of the model's rewritten effects (c3l v12
   * MercModEffect) to m_mod_bytes: per effect the unpacked vertex count, then per unpacked
   * fragment its vertex count, position offsets and lump bytes. */
  void gather_mod(const u8* ee_mem,
                  const u8* effect_ptrs,
                  int effect_count,
                  const struct CtrMercModelData& model);
  /*! (AI-assisted) Unpack what gather_mod copied (from m_mod_bytes[pos]) into the model's mesh. */
  void apply_mod(const struct CtrMercModelData& model, size_t pos);

 public:
  /*! Game thread, before the frame is handed to the render thread: copy the bone matrices the
   * bucket's models use (see handle_model). */
  void snapshot_bones(DmaFollower& dma, CtrRenderState& rs);

  struct Stats {
    int models = 0;
    int missing = 0;
    int draws = 0;
    int bad_bones = 0;
    int env_batches = 0;  // (AI-assisted) envmap_batch: queues drawn
    int sorted_draws = 0;  // (AI-assisted) merc_tex_sort: draws drawn from the queue
    int gen_lod = 0;       // (AI-assisted) merc_gen_lod: models drawn with the generated LOD
    int culled = 0;        // (AI-assisted) cull_merc: draws drawn with back face culling
    int mirrored = 0;      // (AI-assisted) ... marked ones not culled: a bone mirrors
    int unsent_bones = 0;  // (AI-assisted) draws using a bone the game didn't send
    int fogged = 0;        // (AI-assisted, far-fog) merc_fog: draws with the game's fog
  };

 private:
  void handle_setup(const DmaTransfer& setup);
  void handle_model(const DmaTransfer& init, CtrRenderState& rs);
  void hud_clip(float* out, float y_scale) const;
  // (AI-assisted) a bone the model's slots name has a rotation / scale entry of 64 or more, a
  // translation of 1e8 or more, or NaN (garbage: the model isn't drawn)
  static bool bad_bones(const CtrMercMat* const* bones, const u8* slots, int n);
  /*! (AI-assisted, far-fog) merc_fog: the game's fog is more than ~0 somewhere on the model (its
   * sent bones' origins, grown by its size) */
  bool model_fog_visible(const struct CtrMercModelData& model,
                         const CtrMercMat* const* bones,
                         const u8* slots,
                         int n) const;
  bool m_fog_on = false;  // (AI-assisted, far-fog) this bucket's models may get the fog
  /*! (AI-assisted) CtrLevels::find_merc_model of the model name at name (NUL-ended, at most 127
   * bytes), through a small cache: a found model (and its level) for its name while the loaded
   * levels are the same (CtrLevels::set_generation; loading the common file, which doesn't change
   * it, only adds names the cache can't hold: it keeps no misses). The map's lookup compared
   * strings down a tree (~50 models per frame on both threads). */
  const struct CtrMercModelData* find_model(const u8* name, const struct CtrLevelData** lev);
  struct FindSlot {
    u32 hash = 0, gen = 0;
    const struct CtrMercModelData* model = nullptr;
    const struct CtrLevelData* lev = nullptr;
  };
  const struct CtrMercModelData* find_model_named(const u8* name,
                                                  u32 len,
                                                  u32 h,
                                                  u32 gen,
                                                  FindSlot& s,
                                                  const struct CtrLevelData** lev);
  // (AI-assisted) "play as Keira" (CtrKeiraRig.h): her bone matrices, posed from Jak's for each of
  // her models, and the slot table of pointers into them
  CtrMercMat m_rig_mats[128];
  const CtrMercMat* m_rig_slot[256] = {};
  static constexpr int kFindSlots = 64;
  FindSlot m_find[kFindSlots];

  // (AI-assisted) config envmap_batch: the envmap draws of the bucket are queued and drawn
  // together (one skin -> env -> skin program switch pair, and the skin draws keep their uniforms)
  // at its end, or before a draw that would see them differently: one that blends or doesn't
  // write depth (the shine adds Cs * Ad after the base it shines on, which the depth test of the
  // base passes; an opaque draw in front hides it the same way before or after)
  struct QueuedDraw {
    ctr_draw_state st;
    u32 clip, rows, lights;  // offsets in m_qdata
    u32 rows_f24;            // (AI-assisted) offset in m_qf24 (~0u: none)
    u32 bones_id;            // (AI-assisted) the palette's number (ctr_gpu_draw_skinned)
    int palette_count;
    float fade[4];
    int mesh;
    int first_index, index_count;
    u32 model_rank, draw_index;  // merc_tex_sort: the order they're drawn in
  };
  std::vector<float> m_qdata;
  // (AI-assisted) the queued palettes as skin_rows_f24 words (ctr_gpu_pack_skin_rows_f24)
  std::vector<uint32_t> m_qf24;
  std::vector<QueuedDraw> m_q_env;
  u32 queue_floats(const float* src, int n);
  void flush_env();
  // (AI-assisted) config merc_tex_sort: the opaque depth-writing draws of models without blend
  // shapes or rewritten vertices are queued too and drawn draw by draw across the instances of a
  // model (draw 0 of every yeti, then draw 1, ...: each instance keeps its own draw order), so a
  // texture is bound once for all of them. Flushed before a draw that blends or doesn't write
  // depth, before the queued shine.
  std::vector<QueuedDraw> m_q_opaque;
  std::vector<u64> m_q_order;  // (AI-assisted) sort keys (flush_opaque)
  std::vector<const void*> m_q_models;  // model rank -> model
  void flush_opaque();
  void flush_queues();
  // (AI-assisted) merc_gen_lod: per instance (the address of its bone matrices) whether it is
  // drawn with the generated low LOD, and the frame it was last seen
  struct GenLodState {
    bool low = false;
    u32 frame = 0;
  };
  std::unordered_map<u32, GenLodState> m_gen_lod;
  u32 m_frame = 0;

  CtrLevels* m_levels;
  const class CtrEyeRenderer* m_eyes = nullptr;
  // camera from the merc setup packet (low memory of the merc VU program)
  math::Vector4f m_hvdf_offset;
  math::Vector4f m_perspective[4];
  math::Vector4f m_fog;
  float m_clip[16];
  float m_screen[16];  // the GS screen mapping part of m_clip (for HUD models' projection)
  bool m_have_camera = false;
  Stats m_stats;
  // (AI-assisted) the bone snapshot: an array that grows without zeroing the new matrices (a
  // vector's emplace_back value-initializes each one: 64 bytes of zeros, then the copy)
  struct SnapBuf {
    std::unique_ptr<CtrMercMat[]> buf;
    size_t n = 0, cap = 0;
    CtrMercMat* grow(size_t k) {
      if (n + k > cap) {
        cap = std::max<size_t>(2 * cap, n + k + 256);
        std::unique_ptr<CtrMercMat[]> bigger(new CtrMercMat[cap]);
        if (n) {
          memcpy(bigger.get(), buf.get(), n * sizeof(CtrMercMat));
        }
        buf = std::move(bigger);
      }
      CtrMercMat* p = buf.get() + n;
      n += k;
      return p;
    }
    size_t size() const { return n; }
    void clear() { n = 0; }
    const CtrMercMat& operator[](size_t i) const { return buf[i]; }
  };
  SnapBuf m_snap;
  size_t m_snap_pos = 0, m_snap_first = 0;
  // (AI-assisted) rewritten vertices: gather_mod's copies, made with the bone snapshot (game
  // thread) for the models (in DMA order: m_model_ordinal) that have them
  struct ModSnap {
    u32 ordinal;
    size_t pos;  // in m_mod_bytes
  };
  std::vector<ModSnap> m_mod_snaps;
  std::vector<u8> m_mod_bytes;
  size_t m_mod_cursor = 0;
  u32 m_model_ordinal = 0;
  struct ModV {  // an unpacked vertex in MercVertex units
    s16 pos[3];
    s16 st[2];
    s8 nrm[3];
    u8 pad;
  };
  std::vector<ModV> m_mod_tmp;
  // cost, logged once per level: render thread unpack, game thread copy
  double m_mod_ms = 0, m_mod_gather_ms = 0;
  u32 m_mod_frames = 0, m_mod_verts = 0, m_mod_effects = 0, m_mod_bad = 0;
  bool m_mod_frame_counted = false;
  std::string m_mod_logged_for;
  // (AI-assisted) models already logged for a draw whose palette names a bone the game didn't
  // send this frame (it reads the previous model's matrix left in the bone table)
  std::vector<std::string> m_unsent_bone_logged;
  std::string m_mod_last_model;
  bool m_snap_valid = false;
  bool m_snapshot_mode = false;
};
