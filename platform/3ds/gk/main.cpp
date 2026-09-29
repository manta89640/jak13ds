/*!
 * @file main.cpp
 * (AI-assisted)
 * gk for the Nintendo 3DS: sets up the SD card paths and runs the Jak 1 runtime with the null
 * renderer (no graphics yet). Logs go to the bottom screen and to the log file.
 *
 * SD card layout: see docs/3ds-port/3ds_build.md
 *
 * Game arguments: read from sdmc:/3ds/jak1/args.txt (whitespace separated) if it exists,
 * otherwise "-boot" (retail boot, no REPL). "-debug" etc. work like on PC.
 * If sdmc:/3ds/jak1/listener exists, the soc:U socket service is started so the REPL (goalc)
 * can connect over Wi-Fi (costs 1 MB of RAM).
 */

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/graphics/gfx.h"
#include "game/runtime.h"

#include "platform/3ds/port/ctr_port.h"

namespace {
std::vector<std::string> read_game_args() {
  std::vector<std::string> args;
  std::ifstream f(std::string(OPENGOAL_3DS_SD_ROOT) + "/args.txt");
  if (f.good()) {
    std::string a;
    while (f >> a) {
      args.push_back(a);
    }
  } else {
    args.push_back("-boot");
  }
  return args;
}
}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  ctr_platform_init(1);
  printf("OpenGOAL %d.%d for 3DS (%s)\n", versions::GOAL_VERSION_MAJOR,
         versions::GOAL_VERSION_MINOR, ctr_is_new3ds() ? "New 3DS" : "Old 3DS");
  printf("app mem free: %u KB, linear: %u KB\n", ctr_app_mem_free() / 1024,
         ctr_linear_mem_free() / 1024);

  const fs::path data_dir = fs::path(OPENGOAL_3DS_SD_ROOT) / "data";
  if (!file_util::setup_project_path(data_dir)) {
    printf("could not use %s\n", data_dir.string().c_str());
  }

  lg::set_file("jak1");
  lg::set_file_level(lg::level::debug);
  lg::set_stdout_level(lg::level::info);
  lg::set_flush_level(lg::level::warn);
  lg::disable_ansi_colors();
  lg::initialize();

  {
    std::error_code ec;
    if (fs::exists(fs::path(OPENGOAL_3DS_SD_ROOT) / "listener", ec)) {
      int err = ctr_net_init(0x100000);
      lg::info("listener: soc:U init {}", err == 0 ? "ok" : "failed");
    }
  }

  GameLaunchOptions game_options;
  game_options.game_version = GameVersion::Jak1;
  game_options.disable_display = false;  // "display" = the null renderer, which paces vsync
  game_options.server_port = DECI2_PORT;
  Gfx::SetPreferredPipeline(GfxPipeline::Null);

  auto game_args = read_game_args();
  std::vector<const char*> arg_ptrs = {""};  // kmachine starts at argv[1]
  for (auto& a : game_args) {
    arg_ptrs.push_back(a.c_str());
  }

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
