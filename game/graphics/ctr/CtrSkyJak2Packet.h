#pragma once

/*!
 * @file CtrSkyJak2Packet.h
 * Jak II's sky packets for the 3DS renderer: written by the game thread's pc-sky-3ds-* natives
 * (game/mips2c/jak2_functions/native2_sky.cpp) into the sky bucket in place of the stars' sprites
 * and the haze's and clouds' clipped polygons, drawn by CtrSkyJak2 (CtrSky.cpp). A DMA cnt tag
 * whose vif0 is PC_PORT with the kind as its immediate, then one of the structs below.
 */

#include "common/common_types.h"

namespace ctr_sky_jak2 {

enum : u16 { kStars = 0x51, kHaze = 0x52, kClouds = 0x53 };

struct Head {
  // vf31, vf30, vf29, vf28 of the sky code: P = rows[0] x + rows[1] y + rows[2] z + rows[3] w
  float rows[4][4];
  // GS screen x, y = (P.x scale[0], P.y scale[1]) / (P.w scale[2]) + hvdf
  float scale[4];
  float hvdf[4];
  float tex_off[4];  // added to s, t (set-tex-offset)
  u32 giftag[4];     // the polygons' GIF tag (its PRIM)
};
static_assert(sizeof(Head) == 128);

struct Stars {
  Head head;
  u32 seed[8][4];      // sky-work random
  float mat[3][4];     // sky-work star-mat's rows
  s32 colors[16][4];   // sky-work star-colors
  float corner[2][4];  // sky-work star-coords
  float st[2][4];      // sky-work st-coords
};
static_assert(sizeof(Stars) == 624);

struct HazeVertex {  // haze-vertex
  float pos[4];
  float nrm[4];
  float col[4];
};

struct Haze {
  Head head;
  HazeVertex v[36];  // *haze-vert-array*
};
static_assert(sizeof(Haze) == 1856);

struct CloudVertex {  // cloud-vertex
  float pos[4];
  float stq[4];
  float col[4];
  float nrm[4];
  float stq2[4];
  float col2[4];
  float nrm2[4];
};

struct Clouds {
  Head head;
  CloudVertex v[100];  // *cloud-vert-array*
};
static_assert(sizeof(Clouds) == 11328);

}  // namespace ctr_sky_jak2
