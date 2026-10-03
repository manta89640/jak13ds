/*!
 * @file native_ocean.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in ocean_vu0.cpp that the game logic needs. See
 * game/mips2c/mips2c_native.h for the rules (same float operations, grouped the same way, as the
 * mips2c code).
 */

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

//! vitof15 of each s8 height moved to the top byte of a word (pextlb, pextlh): the height * 512
struct HeightFloats {
  float f[256];
  HeightFloats() {
    for (u32 b = 0; b < 256; b++) {
      f[b] = ((float)(s32)(b << 24)) * (1.f / 32768.f);
    }
  }
};

/*!
 * (ocean-interp-wave dest frame): the ocean's wave heights for this frame (32x32 floats at dest),
 * between two of the 64 frames of *ocean-wave-frames* (1024 s8 heights each): frame / 32 and the
 * next one, weighted by frame % 32. The two weights (times 0.333) are also left in *ocean-work* at
 * 60 and 64. Called every frame: ocean-get-height (boats, floating things, Jak in the water) reads
 * the result.
 */
u64 ocean_interp_wave_impl(const NativeArgs& args) {
  static const u32 frames_sym = sym_addr("*ocean-wave-frames*");
  static const u32 work_sym = sym_addr("*ocean-work*");
  static const HeightFloats heights;

  const u32 dest = (u32)args.a[0];
  const u64 frame = args.a[1];
  const s64 i = (s64)frame >> 5;
  const u8* frame_a = gptr(gload<u32>(frames_sym) + (u32)((i & 63) << 10));
  const u8* frame_b = gptr(gload<u32>(frames_sym) + (u32)(((i + 1) & 63) << 10));

  const u32 work = gload<u32>(work_sym);
  gstore<float>(work + 64, 0.03125f * (float)(s32)(frame & 31));
  gstore<float>(work + 60, 1.f - gload<float>(work + 64));
  gstore<float>(work + 60, (float)0.333 * gload<float>(work + 60));
  gstore<float>(work + 64, (float)0.333 * gload<float>(work + 64));
  const float weight_a = gload<float>(work + 60);
  const float weight_b = gload<float>(work + 64);

  // vmulax: acc = a * weight_a (for each of the 256 heights), vmaddy: acc + b * weight_b
  float acc[256];
  for (u32 b = 0; b < 256; b++) {
    acc[b] = heights.f[b] * weight_a;
  }
  float row[32];
  for (u32 r = 0; r < 1024; r += 32) {
    for (u32 k = 0; k < 32; k++) {
      row[k] = acc[frame_a[r + k]] + heights.f[frame_b[r + k]] * weight_b;
    }
    gstore_bytes(dest + 4 * r, row, sizeof(row));
  }
  return 0;
}

/*!
 * (ocean-generate-verts dest heights): the ocean texture's vertices (*ocean-verts*, ocean.gc
 * draw-ocean) from this frame's 32x32 wave heights: per row of heights 8 groups of 4 vertices
 * (vcallms 0, ocean-vu0-block), each stored as 8 quadwords: color 0, the 4 normals' x, color 1,
 * y, color 2, z, color 3, w (the heights). The normal from the height differences (scales x, z of
 * *ocean-vu0-work*), lit by *time-of-day-context*'s lights (vu-lights<-light-group!, colors
 * scaled by *ocean-generate-verts-vector*), color w = 1 / (normal z - 1). Only with config.ini
 * ocean = full on the 3DS (draw-ocean).
 * (AI-assisted) The mips2c version emulates the VU0 macro mode in memory (about 1M ARM
 * instructions per call); this does each vertex's lanes as plain floats, four vertices side by
 * side (independent chains for the VFP pipeline), with the same operations grouped the same way.
 */
u64 ocean_generate_verts_impl(const NativeArgs& args) {
  static const u32 work_sym = sym_addr("*ocean-vu0-work*");
  static const u32 tod_sym = sym_addr("*time-of-day-context*");
  static const u32 block_sym = sym_addr("ocean-vu0-block");
  static const u32 upload_sym = sym_addr("upload-vu0-program");
  static const u32 lights_sym = sym_addr("vu-lights<-light-group!");
  static const u32 vmul_sym = sym_addr("vector*!");
  static const u32 scale_sym = sym_addr("*ocean-generate-verts-vector*");

  u32 dest = (u32)args.a[0];
  const u32 heights = (u32)args.a[1];
  {
    const u32 work = gload<u32>(work_sym);
    const u64 a[8] = {gload<u32>(block_sym), work + 160, args.a[2], args.a[3],
                      args.a[4],             args.a[5],  args.a[6], args.a[7]};
    native_call_goal(gload<u32>(upload_sym), a, args);
  }
  {
    const u64 a[8] = {gload<u32>(work_sym) + 48, gload<u32>(tod_sym) + 156, args.a[2], args.a[3],
                      args.a[4],                 args.a[5],                 args.a[6], args.a[7]};
    native_call_goal(gload<u32>(lights_sym), a, args);
  }
  for (u32 off = 96; off <= 144; off += 16) {
    const u32 v = gload<u32>(work_sym) + off;
    const u64 a[8] = {v, v, gload<u32>(scale_sym), args.a[3], args.a[4], args.a[5], args.a[6],
                      args.a[7]};
    native_call_goal(gload<u32>(vmul_sym), a, args);
  }

  const u32 work = gload<u32>(work_sym);
  float k[4], l0[4], l1[4], l2[4], c0[4], c1[4], c2[4], amb[4];
  gload_q(k, work);  // vf12
  gload_q(l0, work + 48);
  gload_q(l1, work + 64);
  gload_q(l2, work + 80);
  gload_q(c0, work + 96);
  gload_q(c1, work + 112);
  gload_q(c2, work + 128);
  gload_q(amb, work + 144);
  // (acc = vf01 * vf11: the ambient times 1.0)
  float amb1[3];
  for (int i = 0; i < 3; i++) {
    amb1[i] = 1.f * amb[i];
  }

  for (u32 r = 0; r < 32; r++) {
    const u32 row = heights + 128 * r;
    const u32 next = heights + 128 * ((r + 1) & 31);
    for (u32 g = 0; g < 8; g++) {
      // vf02: 4 heights of the row, vf03: the 4 after them (the row's first 4 for the last
      // group), vf04: the next row's 4 under vf02 (only x is read)
      float a[4], b[4], c[4];
      gload_q(a, row + 16 * g);
      gload_q(b, row + 16 * ((g + 1) & 7));
      gload_q(c, next + 16 * g);
      float nx[4], ny[4], nz[4], nw[4], col[4][4];
      for (int v = 0; v < 4; v++) {
        // mula.x acc, vf12, vf02.(v + 1) / vf04.x; mula.z acc, vf12, vf03.v;
        // msub.xz vf24+v, vf12, vf02.v
        const float accx = k[0] * (v < 3 ? a[v + 1] : c[0]);
        const float accz = k[2] * b[v];
        nx[v] = accx - k[0] * a[v];
        nz[v] = accz - k[2] * a[v];
      }
      for (int v = 0; v < 4; v++) {
        const float x2 = nx[v] * nx[v];
        const float z2 = nz[v] * nz[v];
        float y = 1.f - x2;
        ny[v] = y - z2;
        nw[v] = 1.f * a[v];
      }
      float d[4][3];
      for (int v = 0; v < 4; v++) {
        // the light directions' dot products: acc = vf05 * x + vf06 * y, + vf07 * z (before the
        // subw.z below), then clamped to 0 (maxx with vf00.x)
        for (int i = 0; i < 3; i++) {
          float acc = l0[i] * nx[v];
          acc += l1[i] * ny[v];
          d[v][i] = vu_max(acc + l2[i] * nz[v], 0.f);
        }
        nz[v] = nz[v] - 1.f;
      }
      for (int v = 0; v < 4; v++) {
        col[v][3] = 1.f * (1.f / nz[v]);  // div Q, vf00.w, vf2x.z; mulw.w vf2x, vf00, Q
      }
      for (int v = 0; v < 4; v++) {
        // acc = vf01 * vf11, + vf08 * d.x + vf09 * d.y, + vf10 * d.z, then at most vf12.y
        for (int i = 0; i < 3; i++) {
          float acc = amb1[i];
          acc += c0[i] * d[v][0];
          acc += c1[i] * d[v][1];
          col[v][i] = vu_min(acc + c2[i] * d[v][2], k[1]);
        }
      }
      gstore_q(dest, col[0]);
      gstore_q(dest + 16, nx);
      gstore_q(dest + 32, col[1]);
      gstore_q(dest + 48, ny);
      gstore_q(dest + 64, col[2]);
      gstore_q(dest + 80, nz);
      gstore_q(dest + 96, col[3]);
      gstore_q(dest + 112, nw);
      dest += 128;
    }
  }
  return 0;
}

}  // namespace

const NativeImpl ocean_interp_wave = MIPS2C_NATIVE_IMPL(ocean_interp_wave_impl, 0, 0);
const NativeImpl ocean_generate_verts =
    MIPS2C_NATIVE_IMPL(ocean_generate_verts_impl, NATIVE_CALLS_GOAL, 0);

}  // namespace Mips2C::jak1::native
