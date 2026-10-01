/*!
 * @file main.cpp
 * (AI-assisted)
 * gk for the Nintendo 3DS: sets up the SD card paths and runs the Jak 1 runtime with the null
 * renderer (no graphics yet). Logs go to the bottom screen and to the log file.
 *
 * SD card layout: see docs/3ds-port/3ds_build.md
 *
 * Settings: sdmc:/3ds/jak1/config.ini (docs/3ds-port/config_ini.md). Here:
 *   args = <game args> (default "-boot -cbackend": retail boot, no REPL, GOAL code compiled to C)
 *   listener = on: the soc:U socket service, so the REPL (goalc) can connect over Wi-Fi (1 MB RAM)
 *   screenshots = N: a screenshot every N frames to data/log
 *   pad_script = FILE: scripted controller input for tests (relative to sdmc:/3ds/jak1)
 */

#include <cstdio>
#include <sstream>
#include <cstdlib>
#include <malloc.h>
#include <fstream>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/gfx.h"
#include "game/runtime.h"

#include "platform/3ds/port/ctr_port.h"

namespace {
std::vector<std::string> read_game_args() {
  std::vector<std::string> args;
  char v[512];
  if (ctr_config_get("args", v, sizeof(v))) {  // config.ini: args = ...
    std::istringstream f(v);
    std::string a;
    while (f >> a) {
      args.push_back(a);
    }
  } else {
    args.push_back("-boot");
    args.push_back("-cbackend");
  }
  return args;
}
}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  ctr_platform_init(1);
  const fs::path data_dir = fs::path(OPENGOAL_3DS_SD_ROOT) / "data";
  {
    std::error_code ec;
    fs::create_directories(data_dir / "log", ec);
    ctr_stdio_tee((data_dir / "log" / "stdout.log").string().c_str());
  }
  ctr_mem_info mem;
  ctr_get_mem_info(&mem);
  printf("OpenGOAL %d.%d for 3DS (%s, %s)\n", versions::GOAL_VERSION_MAJOR,
         versions::GOAL_VERSION_MINOR, mem.model, mem.is_hbl ? "Homebrew Launcher" : "direct");
  printf("memory: app %u MB, heap %u MB, linear %u MB\n", mem.app_region_total >> 20,
         mem.heap_size >> 20, mem.linear_size >> 20);

  if (!file_util::setup_project_path(data_dir)) {
    printf("could not use %s\n", data_dir.string().c_str());
  }

  // logs on the SD card, flushed line by line so they survive crashes (read them after an
  // emulator run: platform/3ds/tools/run_emu.sh):
  //   data/log/stdout.log  everything printed to the console (GOAL output, lg at info+)
  //   data/log/gk.log      the lg log at debug level
  printf("[gk] project path %s\n", data_dir.string().c_str());
  lg::set_file("gk.log", false, false);
  // Debug lines (thousands during level loads) only with debug_log = on in config.ini: every
  // flush is a write to the SD card, which is slow on hardware (the emulator doesn't notice).
  const bool debug_log = ctr_config_bool("debug_log", 0) != 0;
  lg::set_file_level(debug_log ? lg::level::debug : lg::level::info);
  lg::set_stdout_level(lg::level::info);
  lg::set_flush_level(debug_log ? lg::level::debug : lg::level::warn);
  lg::disable_ansi_colors();
  lg::initialize();
  lg::info("3DS: {} ({}), started as {}", mem.model, mem.is_new3ds ? "New" : "Old",
           mem.is_hbl ? "a 3dsx from the Homebrew Launcher" : "a title (CIA, or an emulator)");
  ctr_boot_mark("6 logs");
  lg::info("3DS memory: application region {} KB ({} KB used), heap {} KB, linear {} KB ({} KB "
           "free)",
           mem.app_region_total / 1024, mem.app_region_used / 1024, mem.heap_size / 1024,
           mem.linear_size / 1024, mem.linear_free / 1024);

  {
    char probe[200];
    ctr_hw_probe(probe, sizeof(probe));
    lg::info("3DS hardware: {}", probe);
  }
  lg::info("3DS: share of core 1 (APT_SetAppCpuTimeLimit): {}%", ctr_core1_share());
  lg::info("3DS: system core (core 1) for the IOP/IO threads: {}",
           ctr_syscore_available() ? "yes (80%, experimental: io_on_system_core)" : "no, core 0");
  {
    int sound_core = 1;
    if (ctr_sound_config(&sound_core)) {
      lg::info("3DS: sound on (config.ini), mixer thread on core {}", sound_core);
    } else {
      lg::info("3DS: sound off (config.ini: sound = on to turn it on)");
    }
  }

  // The EE memory (48 MB) is one malloc; the rest of the runtime needs about 20 MB more.
  constexpr unsigned kHeapNeeded = (unsigned)EE_MAIN_MEM_SIZE + (20u << 20);
  if (mem.heap_size < kHeapNeeded) {
    char text[512];
    snprintf(text, sizeof(text),
             "Not enough memory: %u MB of heap, the game needs %u MB.\n\n"
             "Install the CIA (it asks for the New 3DS\n124 MB mode) and start it from the\n"
             "HOME Menu. A .3dsx started from the\nHomebrew Launcher only gets the memory of\n"
             "the app it runs in; hold R while starting\na game (title takeover) to get more.\n"
             "An Old 3DS doesn't have enough memory.\n",
             mem.heap_size >> 20, kHeapNeeded >> 20);
    lg::error("{}", text);
    ctr_message_wait("Not enough memory", text);
    lg::finish();
    ctr_platform_exit();
    return 1;
  }

  {
    std::error_code ec;
    (void)ec;
    if (ctr_config_bool("listener", 0)) {
      int err = ctr_net_init(0x100000);
      lg::info("listener: soc:U init {}", err == 0 ? "ok" : "failed");
    }
  }

  GameLaunchOptions game_options;
  game_options.game_version = GameVersion::Jak1;
  game_options.disable_display = false;  // "display" = the null renderer, which paces vsync
  game_options.server_port = DECI2_PORT;
  Gfx::SetPreferredPipeline(GfxPipeline::Ctr);
  {
    // config.ini: screenshots = N, a screenshot every N frames to data/log
    char v[32];
    if (ctr_config_get("screenshots", v, sizeof(v))) {
      const int every = std::atoi(v);
      if (every > 0) {
        ctr_gfx::set_screenshots((data_dir / "log").string(), every);
      }
    }
  }

  {
    // config.ini: pad_script = FILE, scripted controller input for tests (game/sce/pad_script.h)
    std::error_code ec;
    char v[256];
    fs::path script;
    if (ctr_config_get("pad_script", v, sizeof(v)) && v[0]) {
      script = v[0] == '/' ? fs::path(v) : fs::path(OPENGOAL_3DS_SD_ROOT) / v;
    }
    if (!script.empty() && fs::exists(script, ec)) {
      setenv("OPENGOAL_PAD_SCRIPT", script.string().c_str(), 1);
      lg::info("pad script: {}", script.string());
    }
  }

  auto game_args = read_game_args();
  std::vector<const char*> arg_ptrs = {""};  // kmachine starts at argv[1]
  for (auto& a : game_args) {
    arg_ptrs.push_back(a.c_str());
  }

  {
    struct mallinfo mi = mallinfo();
    lg::info("3DS: malloc in use at startup: {} KB", mi.uordblks / 1024);
  }
  printf("[gk] starting the runtime\n");
  ctr_boot_mark("7 runtime start");
  RuntimeExitStatus status = RuntimeExitStatus::RUNNING;
  do {
    MasterExit = RuntimeExitStatus::RUNNING;
    status = exec_runtime(game_options, (int)arg_ptrs.size(), arg_ptrs.data());
    lg::info("runtime exited with status {}", (int)status);
  } while (status == RuntimeExitStatus::RESTART_RUNTIME);

  lg::finish();
  ctr_platform_exit();
  return 0;
}
