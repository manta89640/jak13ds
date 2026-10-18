/*!
 * @file CtrRendererJak2Bg.cpp
 * (AI-assisted)
 * Jak II's background buckets for the 3DS renderer (see CtrRendererJak2.h): per level tfrag, tie,
 * shrub (normal, alpha, water), the vis data (BUCKET_2), the ocean. Buckets not set here are
 * skipped.
 */

#include "game/graphics/ctr/CtrRendererJak2.h"

#include "game/graphics/ctr/CtrRenderer.h"

void ctr_jak2_background_buckets(const CtrJak2BucketSet& set, CtrLevels* levels, CtrVram* vram) {
  (void)set;
  (void)levels;
  (void)vram;
}
