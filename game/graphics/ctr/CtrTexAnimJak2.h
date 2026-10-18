#pragma once

/*!
 * @file CtrTexAnimJak2.h
 * (AI-assisted)
 * Jak II texture animation for the 3DS renderer, on the CPU (the PC's TextureAnimator draws them
 * with OpenGL):
 *  - the sky's clouds (CLOUDS_AND_FOG: TextureAnimator::run_clouds) and the generic uploads go to
 *    CtrVram as textures that replace what VRAM holds at their address (CtrVram::set_override),
 *    as the PC's texture pool does;
 *  - (AI-assisted) the CLUT blends (Dark Jak, prison, oracle, nest: ClutBlender) and the fixed
 *    animations (waterfalls, lava, security walls, conveyors, the skull gem...:
 *    run_fixed_animation, CtrTexAnimJak2Defs.h) and the slime go to the animated texture slots:
 *    the level and model draws of a slot's base texture (c3l::texture_anim_slot) use the slot's
 *    GPU texture instead (ctr_gpu_anim_slot_set). Sources from texanim.c3t (next to the .c3l
 *    files), loaded when an animation first runs; an animation not sent for kIdleFrames frames
 *    gives its texture and sources back. Outputs at most 64 x 64 (c3t::kMaxSize); a fixed
 *    animation is redrawn every kFixedRate frames (and only when its input changed); a CLUT blend
 *    only when its weight changes.
 */

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

class CtrVram;

class CtrTexAnimJak2 {
 public:
  CtrTexAnimJak2();
  /*!
   * GENERIC_UPLOAD (texture-anim-pc-upload at rec): CT32 images (the boot logos) go to the VRAM
   * copy; a PSMT8 image waits for the 16 x 16 CT32 CLUT uploaded after it (UPLOAD_CLUT_16_16: clut) and becomes an override
   * texture (the sky's fog gradient), like TextureAnimator's GENERIC_PSMT8. The image data comes
   * from the game thread's snapshot (snapshot_upload), else game memory.
   */
  void generic_upload(const u8* rec, int bytes, const u8* ee_mem, CtrVram* vram, bool clut);
  /*! Game thread (CtrRenderer::prepare_textures): copy a generic upload's image data. */
  void snapshot_upload(const u8* rec, int bytes, const u8* ee_mem);
  /*! Game thread: a new frame's snapshot starts. */
  void snapshot_begin() {
    m_snap_index.clear();
    // (AI-assisted) the boot logos are ~850 KB a frame: don't keep that much heap afterwards
    if (m_snap_bytes.capacity() > (256u << 10) && m_snap_bytes.size() < (64u << 10)) {
      std::vector<u8>().swap(m_snap_bytes);
    }
    m_snap_bytes.clear();
    m_snap_valid = true;
  }
  /*! Render thread: the frame was drawn (the snapshot is used up); idle animations freed. */
  void frame_done();

  /*! (AI-assisted) A fixed animation array (codes 27..40) or a CLUT blend group (22..26), the
   * transfer's data; false: not one of them. */
  bool run(u16 code, const u8* data, int bytes);
  /*! (AI-assisted) SLIME (SlimeInput). */
  void slime(const u8* data, int bytes);
  /*! (AI-assisted) the stats line (render thread ms of the slot animations since the last one) */
  void log_stats();

  /*! CLOUDS_AND_FOG / CLOUDS_HIRES: data is the game's sky input (SkyInput, bytes bytes). */
  void clouds(const u8* data, int bytes, CtrVram* vram);

  static constexpr int kCloudSize = 128;  // kFinalSkyTextureSize
  static constexpr int kCloudLayers = 4;   // kNumSkyNoiseLayers: 16, 32, 64, 128

 private:
  struct Noise {
    std::vector<u8> old_tex, new_tex;
    int dim = 0;
    float last_time = 0;
  };
  // TextureAnimator's random table (kInitialRandomTable) and make_noise_texture
  u8 m_random[8][16];
  int m_random_index = 0;
  void make_noise(u8* dest, int dim);
  Noise m_noise[kCloudLayers];
  std::vector<float> m_acc, m_acc2, m_tmp;
  std::vector<u32> m_out;
  int m_frames = 0;
 public:
  /*! the clouds' render thread time: ms in all runs, runs (taken and reset by the stats line) */
  double m_ms = 0;
  int m_runs = 0;

 private:
  // generic uploads: the snapshot (EE address -> offset in m_snap_bytes) and a PSMT8 image waiting
  // for its CLUT
  std::vector<std::pair<u32, size_t>> m_snap_index;
  std::vector<u8> m_snap_bytes;
  bool m_snap_valid = false;
  const u8* image_data(u32 addr, u32 bytes, const u8* ee_mem) const;
  std::vector<u8> m_t8;
  int m_t8_w = 0, m_t8_h = 0;
  u32 m_t8_dest = 0;
  bool m_t8_waiting = false;
  std::vector<u32> m_t8_rgba;

  // ---- (AI-assisted) the animated texture slots ----
  struct Src {
    std::string name, dgo;
    int w = 0, h = 0, kind = 0;
    u32 offset = 0, size = 0;
    std::vector<u8> data;  // loaded when used (kRgba: RGBA8; kIndex: indices, then 256 colors)
    u64 used_frame = 0;
  };
  bool m_c3t_tried = false;
  std::string m_c3t_path;
  std::vector<Src> m_srcs;
  Src* src(const char* name, const char* dgo, bool load);
  // a slot's output: its GPU texture and texels
  struct Out {
    int slot = -1;
    int w = 0, h = 0;
    int tex = -1;
    std::vector<u32> rgba;
    u64 seen_frame = 0;   // the last frame the game sent it
    u64 drawn_frame = 0;  // the last frame it was redrawn
    std::vector<u8> input;  // the input it was last drawn from (redrawn when it changes)
    float weight = -1.f;    // CLUT blends
    bool upload(int slot_index);
    void release();
  };
  std::map<std::string, Out> m_outs;  // by slot name
  Out* out(const char* name, int w, int h);
  void fixed_anim(const void* def, const u8* in, float time, const u8* vals, int nvals);
  void clut_group(const void* group, const u8* data, int bytes);
  u64 m_frame = 0;
  double m_slot_ms = 0;
  int m_slot_updates = 0;
  std::map<std::string, int> m_slot_names;  // updates per slot since the stats line
  // the slime (TextureAnimator::run_slime): noise layers like the clouds', at 64 x 64
  Noise m_slime_noise[4];
  bool m_slime_init = false;
  std::vector<float> m_slime_acc;

 public:
  static constexpr int kFixedRate = 2;
  static constexpr int kIdleFrames = 300;
};
