#pragma once

/*!
 * @file CtrEye.h
 * (AI-assisted)
 * Eyes for the 3DS renderer. Each frame the game draws the eyes of the characters on screen into
 * VRAM (update-eyes: for each pair of eyes a background, the iris, the pupil and the eyelid as GS
 * sprites), and the merc models map those 32x32 textures on their eye draws. Like the PC port's
 * EyeRenderer, but the sprites are composited on the CPU (a few 32x32 textures per frame) and
 * uploaded as ctr_gpu textures that CtrMercRenderer uses for draws with an eye_id.
 */

#include <deque>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrVram;

class CtrEyeRenderer : public CtrBucketRenderer {
 public:
  static constexpr int kPairs = 20;
  static constexpr int kSlots = kPairs * 2;  // left, right
  static constexpr int kSize = 32;

  CtrEyeRenderer(std::string name, int id, CtrVram* vram);
  ~CtrEyeRenderer() override;
  void render(DmaFollower& dma, CtrRenderState& rs) override;

  /*! Texture for a merc draw's eye_id (the slot the game draws that eye to), -1 if none yet. */
  int texture(int eye_id) const;
  /*!
   * Eye DMA from its GS setup transfer (8 qw) on. The eyes of Jak and Daxter are in the eyes
   * bucket; the eyes of a level's characters follow the level's texture upload in its pris
   * texture bucket (CtrTextureUploadRenderer calls this). Stops at the transfer that restores the
   * GS, or anything unexpected.
   */
  void handle_eye_dma(DmaFollower& dma, CtrRenderState& rs);

 private:
  struct Sprite {
    u32 xyz0[2] = {0, 0};  // GS window coordinates, 1/16 pixel
    u32 xyz1[2] = {0, 0};
    bool valid = false;
  };
  struct Eye {
    int pair = 0, lr = 0;
    bool using_64 = false;
    u64 iris_tex0 = 0, pupil_tex0 = 0, lid_tex0 = 0;
    Sprite iris, pupil, lid;
  };
  struct Source {
    u64 tex0 = 0;
    std::vector<u32> rgba;
    int w = 0, h = 0;
    bool ok = false;
  };

  bool read_pair(DmaFollower& dma, CtrRenderState& rs, std::vector<Eye>* eyes);
  const Source* source(u64 tex0);
  void composite(const Eye& eye);
  void draw_sprite(const Eye& eye, const Sprite& s, const Source* src, bool blend, bool keep_alpha);

  CtrVram* m_vram;
  int m_tex[kSlots];
  u64 m_hash[kSlots];  // of the composited texels
  u64 m_key[kSlots];   // of the sprites and textures they came from
  int m_age[kSlots];   // frames the key didn't change
  std::vector<u32> m_pixels;     // the eye being composited, kSize x kSize, top row first
  std::deque<Source> m_sources;  // textures decoded this frame (deque: pointers stay valid)
  struct Stats {
    int eyes = 0;
    int uploads = 0;
    int bad_dma = 0;
  } m_stats;
};
