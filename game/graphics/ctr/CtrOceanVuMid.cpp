/*!
 * @file CtrOceanVuMid.cpp
 * (AI-assisted)
 * The ocean-mid VU1 programs of the PC renderer (OceanMid_PS2.cpp), compiled for the 3DS
 * renderer's CtrOceanMidVu (CtrOceanVu.h) instead of the OpenGL renderer's OceanMid.
 */

#define OCEAN_VU_FOR_CTR 1
#define OceanMid CtrOceanMidVu
#include "game/graphics/opengl_renderer/ocean/OceanMid_PS2.cpp"
#undef OceanMid
