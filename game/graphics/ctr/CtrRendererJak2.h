#pragma once

/*!
 * @file CtrRendererJak2.h
 * (AI-assisted)
 * Jak II's bucket table for the 3DS renderer (jak2::BucketId). The foreground (merc, sprites,
 * HUD, text, shadows, eyes, sky, texture uploads) is set up in CtrRendererJak2.cpp; the
 * background (tfrag, tie, shrub, the vis data, the ocean) in CtrRendererJak2Bg.cpp.
 */

#include <functional>
#include <memory>

#include "common/common_types.h"
#include "game/graphics/opengl_renderer/buckets.h"

class CtrBucketRenderer;
class CtrLevels;
class CtrVram;

/*! Puts a renderer into the table at a bucket. */
using CtrJak2BucketSet = std::function<void(jak2::BucketId, std::unique_ptr<CtrBucketRenderer>)>;

/*! The background buckets (CtrRendererJak2Bg.cpp). */
void ctr_jak2_background_buckets(const CtrJak2BucketSet& set, CtrLevels* levels, CtrVram* vram);

/*! (AI-assisted, j2gaps) bucket 3 of this frame (DMA tags first_tag..end_tag) asks for a copy of
 * the screen (a menu opens): the frame then clears only the depth (CtrJak2BlitRenderer) */
bool ctr_jak2_frame_copies_screen(const void* ee_mem, u32 first_tag, u32 end_tag);
