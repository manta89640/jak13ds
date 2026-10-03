#pragma once

/*!
 * @file CtrOceanVu.h
 * (AI-assisted)
 * The PS2 ocean's VU1 programs for the 3DS renderer (config.ini ocean = full, see CtrOceanFull.h).
 *
 * The PC renderer emulates them in C++ (game/graphics/opengl_renderer/ocean/OceanMid_PS2.cpp and
 * OceanNear_PS2.cpp). The 3DS renderer compiles those same files for the two classes here
 * (CtrOceanVuMid.cpp, CtrOceanVuNear.cpp: with OCEAN_VU_FOR_CTR, OceanMid.h / OceanNear.h include
 * this header instead of their OpenGL classes). The VU memory, registers and helpers below are the
 * ones the PC classes have; xgkick hands the GS packets to CtrOceanBatch (CtrOceanFull.cpp)
 * instead of CommonOceanRenderer. Jak 1 only (the _jak2 programs are compiled, never called).
 */

#include "common/dma/dma.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"

#include "game/common/vu.h"

class CtrOceanBatch;  // the GS packets of the VU programs (xgkick), see CtrOceanFull.cpp

/*!
 * The ocean-mid VU program (the mid ocean and the transition to the near ocean): OceanMid.
 */
class CtrOceanMidVu {
 public:
  CtrOceanMidVu();
  //! The ocean-mid DMA of the ocean-mid-and-far bucket (OceanMid::run, Jak 1), up to its end tag.
  //! false: not what was expected (the caller skips the rest of the bucket).
  bool run(DmaFollower& dma, CtrOceanBatch* batch);
  //! the native programs (default) or the emulated VU programs (for the host replay test)
  void set_native(bool on) { m_native = on; }

 private:
  // (AI-assisted) the native programs (CtrOceanNative.cpp): the textured pass of call 46 / 73 (a
  // mid ocean tile) and call 107 / 275 (the transition to the near ocean: quads / a fan), as
  // clip-space triangles (the GPU clips, so the VU's clipping and env map pass are left out)
  bool native_ready();
  void native_tile();
  void native_transition(bool fan);
  bool m_native = true;
  int m_native_ok = -1;  // the constants' index table checked this frame: -1 not yet, 0 bad, 1 ok

  void run_call0();
  void run_call0_vu2c();
  void run_call41_vu2c();
  void run_call43_vu2c();
  void run_call46_vu2c();
  void run_call73_vu2c();
  void run_call73_vu2c_jak2();
  void run_call107_vu2c();
  void run_call107_vu2c_jak2();
  void run_call275_vu2c();
  void run_call275_vu2c_jak2();
  void xgkick(u16 addr);

  void run_L26_vu2c();
  void run_L32_vu2c();
  void run_L32_vu2c_jak2();
  void run_L38_vu2c();
  void run_L38_vu2c_jak2();
  void run_L43_vu2c();
  void run_L45_vu2c();

  CtrOceanBatch* m_batch = nullptr;
  bool m_buffer_toggle = false;
  static constexpr int VU1_INPUT_BUFFER_BASE = 0;
  static constexpr int VU1_INPUT_BUFFER_OFFSET = 0x76;

  u16 xtop() {
    m_buffer_toggle = !m_buffer_toggle;
    return get_vu_buffer();
  }

  u16 get_upload_buffer() {
    if (m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  u16 get_vu_buffer() {
    if (!m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  // (deftype ocean-mid-constants (structure), see OceanMid.h
  struct Constants {
    math::Vector4f hmge_scale;
    math::Vector4f inv_hmge_scale;
    math::Vector4f hvdf_offset;
    math::Vector4f fog;
    math::Vector4f constants;
    math::Vector4f constants2;
    u8 drw_fan[16];
    u8 env_fan[16];
    AdGifData drw_adgif;
    u8 drw_texture[16];
    u8 drw_strip_0[16];
    u8 drw_strip_1[16];
    u8 env_adgif[16];
    AdGifData env_texture;
    u8 env_strip[16];
    math::Vector4f env_color;
    math::Vector<u32, 4> index_table[8];
    math::Vector4f pos0;
    math::Vector4f pos1;
    math::Vector4f pos2;
    math::Vector4f pos3;
  } m_constants;
  static_assert(sizeof(Constants) == 0x240);

  enum Vu1Data {
    IN_BUFFER_0 = VU1_INPUT_BUFFER_BASE,    // 0
    IN_BUFFER_1 = VU1_INPUT_BUFFER_OFFSET,  // 0x76
    CONSTANTS = 0x2dd,
  };

  Vf m_vu_data[1024];

  void sq_buffer(Mask mask, const Vf& val, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        m_vu_data[addr].data[i] = val[i];
      }
    }
  }

  void ilw_buffer(Mask mask, u16& dest, u16 addr) {
    ASSERT(addr < 1024);
    switch (mask) {
      case Mask::x:
        dest = m_vu_data[addr].x_as_u16();
        break;
      case Mask::y:
        dest = m_vu_data[addr].y_as_u16();
        break;
      case Mask::z:
        dest = m_vu_data[addr].z_as_u16();
        break;
      case Mask::w:
        dest = m_vu_data[addr].w_as_u16();
        break;
      default:
        ASSERT(false);
    }
  }

  void isw_buffer(Mask mask, u16 src, u16 addr) {
    ASSERT(addr < 1024);
    u32 val32 = src;
    switch (mask) {
      case Mask::x:
        memcpy(&m_vu_data[addr].data[0], &val32, 4);
        break;
      case Mask::y:
        memcpy(&m_vu_data[addr].data[1], &val32, 4);
        break;
      case Mask::z:
        memcpy(&m_vu_data[addr].data[2], &val32, 4);
        break;
      case Mask::w:
        memcpy(&m_vu_data[addr].data[3], &val32, 4);
        break;
      default:
        ASSERT(false);
    }
  }

  void lq_buffer(Mask mask, Vf& dest, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        dest[i] = m_vu_data[addr].data[i];
      }
    }
  }

  struct Vu {
    const Vf vf00;
    Accumulator acc;
    Vf vf01, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf10, vf11, vf12, vf13, vf14, vf15,
        vf16, vf17, vf18, vf19, vf20, vf21, vf22, vf23, vf24, vf25, vf26, vf27, vf28, vf29, vf30,
        vf31;
    u16 vi01, vi02, vi03, vi04, vi05, vi06, vi07, vi08, vi09, vi10, vi11, vi12, vi13, vi14, vi15;
    float Q, P;
    Vu() : vf00(0, 0, 0, 1) {}
  } vu;
};

/*!
 * The ocean-near VU program (the near ocean's waves, 24 m patches around the camera): OceanNear.
 */
class CtrOceanNearVu {
 public:
  CtrOceanNearVu();
  //! The ocean-near DMA after the ocean texture (OceanNear::render_jak1), up to its end tag.
  //! false: not what was expected (the caller skips the rest of the bucket).
  bool run(DmaFollower& dma, CtrOceanBatch* batch);
  //! the native program (default) or the emulated VU program (for the host replay test)
  void set_native(bool on) { m_native = on; }

 private:
  // (AI-assisted) the native program (CtrOceanNative.cpp): call 39's textured pass, in clip space
  void native_patch();
  bool m_native = true;

  void run_call0_vu2c();
  void run_call0_vu2c_jak2();
  void run_call39_vu2c();
  void run_call39_vu2c_jak2();
  void run_L15_vu2c();
  void run_L15_vu2c_jak2();
  void run_L21_vu2c();
  void run_L21_vu2c_jak2();
  void run_L23_vu2c();
  void run_L25_vu2c();
  void run_L25_vu2c_jak2();
  void run_L30_vu2c();
  void run_L32_vu2c();

  CtrOceanBatch* m_batch = nullptr;
  bool m_buffer_toggle = false;
  static constexpr int VU1_INPUT_BUFFER_BASE = 0;
  static constexpr int VU1_INPUT_BUFFER_OFFSET = 0x10;

  void xgkick(u16 addr);

  u16 xtop() {
    m_buffer_toggle = !m_buffer_toggle;
    return get_vu_buffer();
  }

  u16 get_upload_buffer() {
    if (m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  u16 get_vu_buffer() {
    if (!m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  void lq_buffer(Mask mask, Vf& dest, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        dest[i] = m_vu_data[addr].data[i];
      }
    }
  }

  void sq_buffer(Mask mask, const Vf& val, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        m_vu_data[addr].data[i] = val[i];
      }
    }
  }

  void ilw_buffer(Mask mask, u16& dest, u16 addr) {
    ASSERT(addr < 1024);
    switch (mask) {
      case Mask::x:
        dest = m_vu_data[addr].x_as_u16();
        break;
      case Mask::y:
        dest = m_vu_data[addr].y_as_u16();
        break;
      case Mask::z:
        dest = m_vu_data[addr].z_as_u16();
        break;
      case Mask::w:
        dest = m_vu_data[addr].w_as_u16();
        break;
      default:
        ASSERT(false);
    }
  }

  void isw_buffer(Mask mask, u16 src, u16 addr) {
    ASSERT(addr < 1024);
    u32 val32 = src;
    switch (mask) {
      case Mask::x:
        memcpy(&m_vu_data[addr].data[0], &val32, 4);
        break;
      case Mask::y:
        memcpy(&m_vu_data[addr].data[1], &val32, 4);
        break;
      case Mask::z:
        memcpy(&m_vu_data[addr].data[2], &val32, 4);
        break;
      case Mask::w:
        memcpy(&m_vu_data[addr].data[3], &val32, 4);
        break;
      default:
        ASSERT(false);
    }
  }

  Vf m_vu_data[1024];

  struct Vu {
    const Vf vf00;

    Accumulator acc;
    Vf vf01, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf10, vf11, vf12, vf13, vf14, vf15,
        vf16, vf17, vf18, vf19, vf20, vf21, vf22, vf23, vf24, vf25, vf26, vf27, vf28, vf29, vf30,
        vf31;
    u16 vi01, vi02, vi03, vi04, vi05, vi06, vi07, vi08, vi09, vi10, vi11, vi12, vi13, vi14;

    float P, Q;
    Vu() : vf00(0, 0, 0, 1) {}

  } vu;
};
