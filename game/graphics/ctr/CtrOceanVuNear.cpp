/*!
 * @file CtrOceanVuNear.cpp
 * (AI-assisted)
 * The ocean-near VU1 programs of the PC renderer (OceanNear_PS2.cpp), compiled for the 3DS
 * renderer's CtrOceanNearVu (CtrOceanVu.h) instead of the OpenGL renderer's OceanNear.
 */

#define OCEAN_VU_FOR_CTR 1
#define OceanNear CtrOceanNearVu
#include "game/graphics/opengl_renderer/ocean/OceanNear_PS2.cpp"
#undef OceanNear
