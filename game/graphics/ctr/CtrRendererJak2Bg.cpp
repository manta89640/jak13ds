/*!
 * @file CtrRendererJak2Bg.cpp
 * (AI-assisted)
 * Jak II's background buckets for the 3DS renderer (see CtrRendererJak2.h): per level tfrag, tie,
 * shrub (normal, alpha, water), the vis data (BUCKET_2), the ocean. Buckets not set here are
 * skipped.
 *
 * The level backgrounds come from the .c3l files (out/jak2/c3l, ctr_level_converter on the Jak II
 * fr3 files), drawn by CtrTfragRenderer::render_jak2: one renderer per level slot (*level*
 * draw-level 0..5), shared by all of the slot's background buckets. The first of them with the
 * level's camera in a frame draws the whole level (tfrag, tie, shrub of every kind, opaque and
 * translucent), as Jak 1's tfrag buckets do. Jak II's buckets are ordered by kind across the slots
 * (all slots' tfrag groups, then all shrub groups, then all alpha groups), so a slot whose level
 * only has shrubs or translucent tfrag is still drawn, just later in the frame.
 */

#include "game/graphics/ctr/CtrRendererJak2.h"
#include "game/graphics/ctr/CtrOcean.h"
#include "game/graphics/ctr/CtrSettings.h"

#include <cstring>
#include <string>

#include "common/dma/dma.h"
#include "common/dma/dma_chain_read.h"

#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrRenderer.h"

namespace {

/*! A background bucket of one level slot: hands its DMA to the slot's CtrTfragRenderer. */
class CtrJak2BgBucket : public CtrBucketRenderer {
 public:
  CtrJak2BgBucket(std::string name, int id, std::shared_ptr<CtrTfragRenderer> tfrag, int slot)
      : CtrBucketRenderer(std::move(name), id), m_tfrag(std::move(tfrag)), m_slot(slot) {}
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    m_tfrag->render_jak2(dma, rs, m_slot);
  }

 private:
  std::shared_ptr<CtrTfragRenderer> m_tfrag;
  int m_slot;
};

/*!
 * BUCKET_2 (add-pc-port-background-data, like the PC's VisDataHandler): per level slot a PC_PORT
 * transfer with the slot's visibility string (128 qw for an active level, else 1 qw of zeros),
 * vif0 = *fog-color*, each followed by an empty next tag; then add-pc-camera-data's packet.
 */
class CtrJak2VisBucket : public CtrBucketRenderer {
 public:
  CtrJak2VisBucket(std::string name, int id, CtrLevels* levels)
      : CtrBucketRenderer(std::move(name), id), m_levels(levels) {}
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    auto& vis = m_levels->jak2_vis();
    for (auto& v : vis.valid) {
      v = false;
    }
    if (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();  // the bucket's start
    }
    for (int i = 0; i < CtrJak2Vis::kSlots && dma.current_tag_offset() != rs.next_bucket; i++) {
      auto t = dma.read_and_advance();
      if (t.vifcode1().kind != VifCode::Kind::PC_PORT) {
        break;
      }
      // the fog color of the level background (the PC's fog_color comes from here too)
      const u32 fog = t.vif0();
      memcpy(rs.fog_color, &fog, 4);
      if (t.size_bytes == 2048) {
        // an active level's string. All zero is valid too, as on the PC and the PS2: nothing of
        // the level can be seen from here (vis-clear's zeros while its string loads hide it as
        // well, as on the PS2). Seen from the slums, ctyindb's string is all zero: not drawn.
        memcpy(vis.bits[i], t.data, 2048);
        vis.valid[i] = true;
      }
      if (dma.current_tag_offset() != rs.next_bucket) {
        dma.read_and_advance();  // next
      }
    }
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();  // the camera packet (for the collision renderer on PC)
    }
  }

 private:
  CtrLevels* m_levels;
};

}  // namespace

void ctr_jak2_background_buckets(const CtrJak2BucketSet& set, CtrLevels* levels, CtrVram* vram) {
  (void)vram;
  using B = jak2::BucketId;
  set(B::BUCKET_2, std::make_unique<CtrJak2VisBucket>("vis", (int)B::BUCKET_2, levels));
  // (AI-assisted) the ocean: the packet of ocean-3ds-packet (goal_src/jak2/engine/gfx/ocean/
  // ocean.gc, TARGET_3DS) in ocean-near, drawn from the map like Jak 1's (CtrOcean.h; ocean = full
  // is the flat ocean too: the PS2 ocean's DMA isn't sent)
  if (ctr_settings().ocean) {
    set(B::OCEAN_NEAR, std::make_unique<CtrOceanRenderer>("ocean", (int)B::OCEAN_NEAR));
  }
  // the bucket of slot i: first + (second - first) * i (the slots' groups have the same layout)
  auto for_slot = [](B l0, B l1, int i) { return (B)((int)l0 + ((int)l1 - (int)l0) * i); };
  struct Kind {
    B l0, l1;
  };
  static const Kind kKinds[] = {
      {B::TFRAG_L0_TFRAG, B::TFRAG_L1_TFRAG},       {B::TIE_L0_TFRAG, B::TIE_L1_TFRAG},
      {B::ETIE_L0_TFRAG, B::ETIE_L1_TFRAG},         {B::TFRAG_S_L0_TFRAG, B::TFRAG_S_L1_TFRAG},
      {B::TIE_S_L0_TFRAG, B::TIE_S_L1_TFRAG},       {B::ETIE_S_L0_TFRAG, B::ETIE_S_L1_TFRAG},
      {B::TIE_V_L0_TFRAG, B::TIE_V_L1_TFRAG},       {B::SHRUB_L0_SHRUB, B::SHRUB_L1_SHRUB},
      {B::SHRUB_N_L0_SHRUB, B::SHRUB_N_L1_SHRUB},   {B::BILLBOARD_L0_SHRUB, B::BILLBOARD_L1_SHRUB},
      {B::SHRUB_V_L0_SHRUB, B::SHRUB_V_L1_SHRUB},   {B::SHRUB_NT_L0_SHRUB, B::SHRUB_NT_L1_SHRUB},
      {B::TFRAG_T_L0_ALPHA, B::TFRAG_T_L1_ALPHA},   {B::TIE_T_L0_ALPHA, B::TIE_T_L1_ALPHA},
      {B::ETIE_T_L0_ALPHA, B::ETIE_T_L1_ALPHA},     {B::TFRAG_ST_L0_ALPHA, B::TFRAG_ST_L1_ALPHA},
      {B::TIE_ST_L0_ALPHA, B::TIE_ST_L1_ALPHA},     {B::ETIE_ST_L0_ALPHA, B::ETIE_ST_L1_ALPHA},
      {B::TFRAG_W_L0_WATER, B::TFRAG_W_L1_WATER},   {B::TIE_W_L0_WATER, B::TIE_W_L1_WATER},
      {B::ETIE_W_L0_WATER, B::ETIE_W_L1_WATER},     {B::TIE_SW_L0_WATER, B::TIE_SW_L1_WATER},
      {B::TFRAG_WS_L0_WATER, B::TFRAG_WS_L1_WATER},
  };
  // (L2's water group has TIE_ESW_L2_WATER where the others have ETIE_SW: the same slot, left out
  // like ETIE_SW, which the game doesn't use)
  for (int i = 0; i < CtrJak2Vis::kSlots; i++) {
    // "l<i>-tfrag": the level phase of gpu_breakdown, gpu_profile's "level" mode (l0/l1)
    auto tfrag = std::make_shared<CtrTfragRenderer>(
        "l" + std::to_string(i) + "-tfrag", (int)for_slot(B::TFRAG_L0_TFRAG, B::TFRAG_L1_TFRAG, i),
        levels);
    // (AI-assisted, j2vis) where the slot's hidden prototype names come (c3l v20)
    tfrag->set_jak2_proto_buckets((int)for_slot(B::TIE_L0_TFRAG, B::TIE_L1_TFRAG, i),
                                  (int)for_slot(B::SHRUB_L0_SHRUB, B::SHRUB_L1_SHRUB, i));
    for (const auto& k : kKinds) {
      const B id = for_slot(k.l0, k.l1, i);
      set(id, std::make_unique<CtrJak2BgBucket>(tfrag->name(), (int)id, tfrag, i));
    }
  }
}
