/*!
 * @file null.cpp
 * Null renderer: accepts DMA chains and textures and drops them, and paces vsync at 60 Hz.
 * (AI-assisted)
 */

#include "null.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/log/log.h"

#include "game/kernel/common/kboot.h"

#if defined(__3DS__) && OG3DS_GAME_JAK2
#include "platform/3ds/port/ctr_port.h"
#endif

namespace {

using clock_type = std::chrono::steady_clock;

struct NullGfxState {
  clock_type::time_point next_frame;
  bool started = false;
  u32 frame_idx = 0;
} g_null_state;

int null_init(GfxGlobalSettings& /*settings*/) {
  g_null_state = NullGfxState();
  lg::info("Null renderer: no display, 60 Hz vsync pacing");
  return 0;
}

std::shared_ptr<GfxDisplay> null_make_display(int /*w*/,
                                              int /*h*/,
                                              const char* /*title*/,
                                              GfxGlobalSettings& /*settings*/,
                                              GameVersion /*version*/,
                                              bool /*is_main*/) {
  return nullptr;
}

void null_exit() {}

/*!
 * Called by the game (sceGsSyncV) once per frame. Sleeps until the next 60 Hz frame boundary.
 * Gfx::vsync fires the vblank callback (IOP) before calling this.
 */
u32 null_vsync() {
  // (AI-assisted) OPENGOAL_NULL_GFX_FPS=30 (host): pace at 30 Hz like the 3DS's fps cap, so the
  // game runs with a time ratio of 2 (tests of frame rate dependent behaviour)
  static const auto kFrame = [] {
    int fps = 60;
#ifndef __3DS__
    if (const char* e = getenv("OPENGOAL_NULL_GFX_FPS")) {
      fps = std::max(1, std::min(60, atoi(e)));
      lg::info("Null renderer: {} Hz vsync pacing (OPENGOAL_NULL_GFX_FPS)", fps);
    }
#elif OG3DS_GAME_JAK2
    // (AI-assisted) the Jak II 3DS build: 30 Hz like Jak 1's 3DS frame rate cap (and the host
    // timing runs, OPENGOAL_NULL_GFX_FPS=30); config.ini null_fps = 60 for 60 Hz
    {
      char v[16];
      fps = 30;
      if (ctr_config_get("null_fps", v, sizeof(v))) {
        fps = std::max(1, std::min(60, atoi(v)));
      }
      lg::info("Null renderer: {} Hz vsync pacing", fps);
    }
#endif
    return std::chrono::nanoseconds(1000000000LL / fps);
  }();
  const auto now = clock_type::now();
  if (!g_null_state.started) {
    g_null_state.started = true;
    g_null_state.next_frame = now + kFrame;
  } else {
    g_null_state.next_frame += kFrame;
    if (g_null_state.next_frame < now - 4 * kFrame) {
      // we're running behind (slow frame / debugger): don't try to catch up
      g_null_state.next_frame = now;
    }
  }
  if (null_gfx_no_sync()) {
    g_null_state.next_frame = now;  // pacing restarts from here when the file goes away
  } else if (MasterExit == RuntimeExitStatus::RUNNING) {
    std::this_thread::sleep_until(g_null_state.next_frame);
  }
  g_null_state.frame_idx++;
  return g_null_state.frame_idx & 1;
}

u32 null_sync_path() {
  return 0;
}

void null_send_chain(const void* /*data*/, u32 /*offset*/) {}
void null_texture_upload_now(const u8* /*tpage*/, int /*mode*/, u32 /*s7_ptr*/) {}
void null_texture_relocate(u32 /*dst*/, u32 /*src*/, u32 /*format*/) {}
void null_set_levels(const std::vector<std::string>& /*levels*/) {}
void null_set_active_levels(const std::vector<std::string>& /*levels*/) {}
void null_force_reload_all() {}
void null_force_reload_level(const std::string& /*level*/) {}
void null_force_reload_common() {}
void null_set_pmode_alp(float /*val*/) {}

}  // namespace

bool null_gfx_no_sync() {
#ifdef __3DS__
  return false;
#else
  static const char* path = getenv("OPENGOAL_NULL_GFX_NO_SYNC");
  static thread_local u32 calls = 0;
  static thread_local bool on = false;
  if (!path) {
    return false;
  }
  if ((calls++ & 63) == 0) {
    FILE* f = fopen(path, "rb");
    on = f != nullptr;
    if (f) {
      fclose(f);
    }
  }
  return on;
#endif
}

const GfxRendererModule gRendererNull = {
    null_init,                 // init
    null_make_display,         // make_display
    null_exit,                 // exit
    null_vsync,                // vsync
    null_sync_path,            // sync_path
    null_send_chain,           // send_chain
    null_texture_upload_now,   // texture_upload_now
    null_texture_relocate,     // texture_relocate
    null_set_levels,           // set_levels
    null_set_active_levels,    // set_active_levels
    null_force_reload_all,     // force_reload_all
    null_force_reload_level,   // force_reload_level
    null_force_reload_common,  // force_reload_common
    null_set_pmode_alp,        // set_pmode_alp
    GfxPipeline::Null,         // pipeline
    "Null"                     // name
};
