#pragma once

/*!
 * @file CtrTexAnimJak2.h
 * (AI-assisted)
 * Jak II texture animation for the 3DS renderer, on the CPU (the PC's TextureAnimator draws them
 * with OpenGL). Done so far: the sky's clouds (CLOUDS_AND_FOG: TextureAnimator::run_clouds).
 * Results go to CtrVram as textures that replace what VRAM holds at their address
 * (CtrVram::set_override), as the PC's texture pool does.
 */

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
    m_snap_bytes.clear();
    m_snap_valid = true;
  }
  /*! Render thread: the frame was drawn (the snapshot is used up). */
  void frame_done() { m_snap_valid = false; }

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
};
