/*!
 * @file runtime.cpp
 * Setup and launcher for the runtime.
 */

#include "common/common_types.h"
#if defined(__3DS__)
#include <malloc.h>
#include <unistd.h>
#elif defined(OS_POSIX)
#include <unistd.h>

#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include "mach/mach.h"
#include "mach/mach_vm.h"
#endif
#elif _WIN32
#include <io.h>

#include "third-party/mman/mman.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <chrono>
#include <cstring>
#include <thread>

#include "runtime.h"

#include "common/cross_os_debug/xdbg.h"
#include "common/global_profiler/GlobalProfiler.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/external/discord.h"
#include "game/graphics/gfx.h"
#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/kdsnetm.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kmemcard.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kboot.h"
#include "game/kernel/jak1/kdgo.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#if !OPENGOAL_ONLY_JAK1
#include "game/kernel/jak2/kboot.h"
#include "game/kernel/jak2/kdgo.h"
#include "game/kernel/jak2/klisten.h"
#include "game/kernel/jak2/kscheme.h"
#if !OPENGOAL_NO_JAK3
#include "game/kernel/jak3/kboot.h"
#include "game/kernel/jak3/kdgo.h"
#include "game/kernel/jak3/klisten.h"
#include "game/kernel/jak3/kscheme.h"
#include "game/kernel/jakx/kboot.h"
#endif
#endif
#include "game/overlord/common/fake_iso.h"
#include "game/overlord/common/iso.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/srpc.h"
#include "game/overlord/common/ssound.h"
#include "game/overlord/jak1/dma.h"
#include "game/overlord/jak1/fake_iso.h"
#include "game/overlord/jak1/iso.h"
#include "game/overlord/jak1/iso_queue.h"
#include "game/overlord/jak1/overlord.h"
#include "game/overlord/jak1/ramdisk.h"
#include "game/overlord/jak1/srpc.h"
#include "game/overlord/jak1/stream.h"
#if !OPENGOAL_ONLY_JAK1
#include "game/overlord/jak2/dma.h"
#include "game/overlord/jak2/iso_cd.h"
#include "game/overlord/jak2/iso_queue.h"
#include "game/overlord/jak2/overlord.h"
#include "game/overlord/jak2/spustreams.h"
#include "game/overlord/jak2/srpc.h"
#include "game/overlord/jak2/ssound.h"
#include "game/overlord/jak2/stream.h"
#include "game/overlord/jak2/streamlist.h"
#include "game/overlord/jak2/vag.h"
#if !OPENGOAL_NO_JAK3
#include "game/overlord/jak3/overlord.h"
#endif
#endif
#include "game/system/Deci2Server.h"
#include "game/system/iop_thread.h"
#include "sce/deci2.h"
#include "sce/iop.h"
#include "sce/libcdvd_ee.h"
#include "sce/sif_ee.h"
#include "system/SystemThread.h"
#ifdef __3DS__
#include "platform/3ds/port/ctr_port.h"
#endif

u8* g_ee_main_mem = nullptr;
u8* g_ee_main_mem_exec = nullptr;
std::thread::id g_main_thread_id = std::thread::id();
GameVersion g_game_version = GameVersion::Jak1;
BackgroundWorker g_background_worker;
int g_server_port = DECI2_PORT;

namespace {

int g_argc = 0;
const char** g_argv = nullptr;

/*!
 * SystemThread function for running the DECI2 communication with the GOAL compiler.
 */

void deci2_runner(SystemThreadInterface& iface) {
  // callback function so the server knows when to give up and shutdown
  std::function<bool()> shutdown_callback = [&]() { return iface.get_want_exit(); };

  // create and register server
  Deci2Server server(shutdown_callback, deci2_port_for_game((int)g_game_version));
  ee::LIBRARY_sceDeci2_register(&server);

  // now its ok to continue with initialization
  iface.initialization_complete();

  // in our own thread, wait for the EE to register the first protocol driver
  lg::debug("[DECI2] Waiting for EE to register protos");
  if (!server.wait_for_protos_ready()) {
    // requested shutdown before protos became ready.
    return;
  }
  // then allow the server to accept connections
  bool server_ok = server.init_server();
  if (!server_ok) {
    lg::error("[DECI2] failed to initialize, REPL will not work.");
  }

  lg::debug("[DECI2] Waiting for listener...");
  bool saw_listener = false;
  while (!iface.get_want_exit()) {
    if (server_ok && server.is_client_connected()) {
      if (!saw_listener) {
        lg::debug("[DECI2] Connected!");
      }
      saw_listener = true;
      // we have a listener, run!
      server.read_data();
    } else {
      // no connection yet.  Do a sleep so we don't spam checking the listener.
      std::this_thread::sleep_for(std::chrono::microseconds(50000));
    }
  }
}

// EE System

/*!
 * SystemThread Function for the EE (PS2 Main CPU)
 */
void ee_runner(SystemThreadInterface& iface) {
  prof().root_event();
  // Allocate Main RAM. Must have execute enabled.
  // Apple Silicon uses separate writable and executable mappings for EE memory.
#if defined(__3DS__)
  // No mmap. GOAL code is compiled to C, so the memory doesn't need to be executable.
  g_ee_main_mem = (u8*)memalign(4096, EE_MAIN_MEM_SIZE);
  if (!g_ee_main_mem) {
    lg::error("Failed to allocate {} MB of main memory!", EE_MAIN_MEM_SIZE >> 20);
    iface.initialization_complete();
    return;
  }
#else
  if (EE_MEM_LOW_MAP) {
    g_ee_main_mem =
        (u8*)mmap((void*)0x10000000, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
#ifdef __APPLE__
                  // has no map_populate
                  MAP_ANONYMOUS | MAP_32BIT | MAP_PRIVATE, 0, 0);
#else
                  MAP_ANONYMOUS | MAP_32BIT | MAP_PRIVATE | MAP_POPULATE, 0, 0);
#endif
  } else {
#if defined(__APPLE__) && defined(__aarch64__)
    // mach_vm_remap needs an anonymous writable mapping
    g_ee_main_mem = (u8*)mmap((void*)EE_MAIN_MEM_MAP, EE_MAIN_MEM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#else
    g_ee_main_mem =
        (u8*)mmap((void*)EE_MAIN_MEM_MAP, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
#endif
  }

  if (g_ee_main_mem == (u8*)(-1)) {
    lg::debug("Failed to initialize main memory! {}", strerror(errno));
    iface.initialization_complete();
    return;
  }
#endif

  g_ee_main_mem_exec = g_ee_main_mem;

#if defined(__APPLE__) && defined(__aarch64__)
  // map the same pages again with read and execute access
  {
    mach_vm_address_t exec_addr = 0;
    vm_prot_t cur_prot = 0, max_prot = 0;
    kern_return_t kr = mach_vm_remap(
        mach_task_self(), &exec_addr, EE_MAIN_MEM_SIZE, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
        (mach_vm_address_t)g_ee_main_mem, false, &cur_prot, &max_prot, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
      lg::error("Cannot map the executable view of main memory: mach_vm_remap returned {}",
                (int)kr);
      iface.initialization_complete();
      return;
    }
    kr = mach_vm_protect(mach_task_self(), exec_addr, EE_MAIN_MEM_SIZE, false,
                         VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
      lg::error("Cannot make main memory executable: mach_vm_protect returned {}", (int)kr);
      iface.initialization_complete();
      return;
    }
    g_ee_main_mem_exec = (u8*)exec_addr;
    lg::debug("Main memory executable view at 0x{:016x} (delta 0x{:x})", (u64)g_ee_main_mem_exec,
              (u64)(g_ee_main_mem_exec - g_ee_main_mem));
  }
#endif

  lg::debug("Main memory mapped at 0x{:016x}", (u64)(g_ee_main_mem));
  lg::debug("Main memory size 0x{:x} bytes ({:.3f} MB)", EE_MAIN_MEM_SIZE,
            (double)EE_MAIN_MEM_SIZE / (1 << 20));

  lg::debug("[EE] Initialization complete!");
  iface.initialization_complete();

  lg::debug("[EE] Run!");
  memset((void*)g_ee_main_mem, 0, EE_MAIN_MEM_SIZE);

  // prevent access to the first 512 kB of memory.
  // On the PS2 this is the kernel and can't be accessed either.
  // this may not work well on systems with a page size > 1 MB.
#if !defined(__3DS__)
  mprotect((void*)g_ee_main_mem, EE_MAIN_MEM_LOW_PROTECT, PROT_NONE);
#endif
  fileio_init_globals();
  jak1::kboot_init_globals();
#if !OPENGOAL_ONLY_JAK1
  jak2::kboot_init_globals();
#if !OPENGOAL_NO_JAK3
  jak3::kboot_init_globals();
#endif
#endif

  kboot_init_globals_common();
  kdgo_init_globals();
  jak1::kdgo_init_globals();
#if !OPENGOAL_ONLY_JAK1
  jak2::kdgo_init_globals();
#if !OPENGOAL_NO_JAK3
  jak3::kdgo_init_globals();
#endif
#endif

  kdsnetm_init_globals_common();
  klink_init_globals();

  kmachine_init_globals_common();
  jak1::kscheme_init_globals();
#if !OPENGOAL_ONLY_JAK1
  jak2::kscheme_init_globals();
#if !OPENGOAL_NO_JAK3
  jak3::kscheme_init_globals();
#endif
#endif
  kscheme_init_globals_common();
  kmalloc_init_globals_common();

  klisten_init_globals();
  jak1::klisten_init_globals();
#if !OPENGOAL_ONLY_JAK1
  jak2::klisten_init_globals();
#if !OPENGOAL_NO_JAK3
  jak3::klisten_init_globals();
#endif

  jak2::vag_init_globals();

  jak2::init_globals_streamlist();
#endif

  kmemcard_init_globals();
  kprint_init_globals_common();

  // Added for OpenGOAL's debugger
  xdbg::allow_debugging();

  switch (g_game_version) {
    case GameVersion::Jak1:
      jak1::goal_main(g_argc, g_argv);
      break;
#if !OPENGOAL_ONLY_JAK1
    case GameVersion::Jak2:
      jak2::goal_main(g_argc, g_argv);
      break;
#if !OPENGOAL_NO_JAK3
    case GameVersion::Jak3:
      jak3::goal_main(g_argc, g_argv);
      break;
    case GameVersion::JakX:
      jakx::goal_main(g_argc, g_argv);
      break;
#endif
#endif
    default:
      ASSERT_MSG(false, "Unsupported game version");
  }
  lg::debug("[EE] Done!");

  //  // kill the IOP todo
  iop::LIBRARY_kill();

  // after main returns, trigger a shutdown.
  iface.trigger_shutdown();
}

/*!
 * SystemThread Function for the EE Worker Thread (general purpose background tasks from the EE to
 * be non-blocking)
 */
void ee_worker_runner(SystemThreadInterface& iface) {
  iface.initialization_complete();
  while (!iface.get_want_exit()) {
    const auto queues_weres_empty = !g_background_worker.process_queues();
    if (queues_weres_empty) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

/*!
 * SystemThread function for running the IOP (separate I/O Processor)
 */
void iop_runner(SystemThreadInterface& iface, GameVersion version) {
  prof().root_event();
  prof().begin_event("iop-init");
  IOP iop;
  lg::debug("[IOP] Restart!");
  iop.reset_allocator();
  ee::LIBRARY_sceSif_register(&iop);
  iop::LIBRARY_register(&iop);
#ifdef __3DS__
  // (AI-assisted) wake the IOP for its vblank handler: it doesn't poll every millisecond any more
  Gfx::register_vsync_callback([&iop]() {
    iop.kernel.signal_vblank();
    iop.signal_run_iop();
  });
#else
  Gfx::register_vsync_callback([&iop]() { iop.kernel.signal_vblank(); });
#endif

  if (version != GameVersion::Jak3 && version != GameVersion::JakX) {
    jak1::dma_init_globals();
    iso_init_globals();
    jak1::iso_init_globals();
    fake_iso_init_globals();
    jak1::fake_iso_init_globals();
    jak1::iso_queue_init_globals();
    jak1::ramdisk_init_globals();
    sbank_init_globals();
    // soundcommon
    jak1::srpc_init_globals();
    srpc_init_globals();
    ssound_init_globals();
    jak1::stream_init_globals();
#if !OPENGOAL_ONLY_JAK1
    jak2::dma_init_globals();
    jak2::iso_init_globals();
    jak2::iso_cd_init_globals();
    jak2::iso_queue_init_globals();
    jak2::spusstreams_init_globals();
    jak2::srpc_init_globals();
    jak2::ssound_init_globals();
    jak2::stream_init_globals();
#endif
  }

  prof().end_event();
  iface.initialization_complete();

  lg::debug("[IOP] Wait for OVERLORD to start...");
  {
    auto p = scoped_prof("iop-wait-for-ee");
    iop.wait_for_overlord_start_cmd();
  }
  if (iop.status == IOP_OVERLORD_INIT) {
    lg::debug("[IOP] Run!");
  } else {
    lg::debug("[IOP] Shutdown!");
    return;
  }

  iop.reset_allocator();

  // init

  bool complete = false;
  {
    auto p = scoped_prof("overlord-start");
    switch (version) {
      case GameVersion::Jak1:
        jak1::start_overlord_wrapper(iop.overlord_argc, iop.overlord_argv, &complete);
        break;
#if !OPENGOAL_ONLY_JAK1
      case GameVersion::Jak2:
        jak2::start_overlord_wrapper(iop.overlord_argc, iop.overlord_argv, &complete);
        break;
#if !OPENGOAL_NO_JAK3
      case GameVersion::Jak3:
      case GameVersion::JakX:
        jak3::start_overlord_wrapper(&complete);
        break;
#endif
#endif
      default:
        ASSERT_NOT_REACHED();
    }
  }

  {
    auto p = scoped_prof("overlord-wait-for-init");
    while (complete == false) {
      iop.kernel.dispatch();
#ifdef __3DS__
      // (AI-assisted) this loop polls: let lower priority threads run (dispatch doesn't sleep)
      std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
    }
  }

  // unblock the EE, the overlord is set up!
  iop.signal_overlord_init_finish();

  // IOP Kernel loop
  while (!iface.get_want_exit() && !iop.want_exit) {
    // prof().root_event();
    // The IOP scheduler informs us of how many microseconds are left until it has something to do.
    // So we can wait for that long or until something else needs it to wake up.
    auto wait_duration = iop.kernel.dispatch();
    if (wait_duration) {
      iop.wait_run_iop(*wait_duration);
    }
  }

  Gfx::clear_vsync_callback();
}
}  // namespace

/*!
 * SystemThread function for running NothingTM.
 */
void null_runner(SystemThreadInterface& iface) {
  iface.initialization_complete();
}

/*!
 * Main function to launch the runtime.
 * GOAL kernel arguments are currently ignored.
 */
RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv) {
  prof().root_event();
  g_argc = argc;
  g_argv = argv;
  g_main_thread_id = std::this_thread::get_id();

  bool enable_display = !game_options.disable_display;
  g_game_version = game_options.game_version;
  g_server_port = game_options.server_port;

  gStartTime = time(nullptr);
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("startup::exec_runtime::init_discord_rpc");
    init_discord_rpc();
  }

  // initialize graphics first - the EE code will upload textures during boot and we
  // want the graphics system to catch them.
  {
    auto p = scoped_prof("startup::exec_runtime::init_gfx");
    if (enable_display) {
      Gfx::Init(g_game_version);
    }
  }

  // step 1: sce library prep
  {
    auto p = scoped_prof("startup::exec_runtime::library_prep");
    iop::LIBRARY_INIT();
    ee::LIBRARY_INIT_sceCd();
    ee::LIBRARY_INIT_sceDeci2();
    ee::LIBRARY_INIT_sceSif();
  }

  // step 2: system prep
  prof().begin_event("startup::exec_runtime::system_prep");
  SystemThreadManager tm;
  auto& deci_thread = tm.create_thread("DMP");
  auto& iop_thread = tm.create_thread("IOP");
  auto& ee_thread = tm.create_thread("EE");
  // a general worker thread to perform background operations from the EE thread (to not block the
  // game)
  auto& ee_worker_thread = tm.create_thread("EE-Worker");
  prof().end_event();

#ifdef __3DS__
  // (AI-assisted) Jak II: the IOP thread (the overlord's threads, DGO and file loading) runs on
  // core 2 of the New 3DS, so its ~2 ms per frame in Haven City come off the game's core. Core 2
  // also has the render thread (priority 0x2F, ctr_gpu_citro3d.c) and the level loaders. The IOP
  // must be above the render thread, or the game waits for the IOP while the IOP waits for the
  // render thread (Jak 1 with the IOP at its core 0 priority: 1.4 fps). It mostly sleeps
  // (wait_run_iop), so it takes little from the render thread. Same priority as the sound mixer
  // (CTR_PRIO_SOUND): neither preempts the other, both run in short bursts. Not core 1: Jak 1's
  // boot hung there. config.ini iop_core = 0 puts it back on core 0 (Jak 1: always core 0).
  {
    int iop_core = g_game_version == GameVersion::Jak2 ? 2 : 0;
    char v[8];
    if (ctr_config_get("iop_core", v, sizeof(v)) && (v[0] == '0' || v[0] == '2')) {
      iop_core = v[0] - '0';
    }
    if (iop_core == 2 && ctr_is_new3ds()) {
      iop_thread.set_ctr_placement(2, CTR_PRIO_SOUND);
      lg::info("3DS: IOP thread on core 2");
    }
  }
#endif

  // step 3: start the EE!
  {
    auto p = scoped_prof("startup::exec_runtime::iop-start");
    iop_thread.start([=](SystemThreadInterface& sti) { iop_runner(sti, g_game_version); });
  }
  {
    auto p = scoped_prof("startup::exec_runtime::deci-start");
    deci_thread.start(deci2_runner);
  }
  {
    auto p = scoped_prof("startup::exec_runtime::ee-worker-start");
    ee_worker_thread.start(ee_worker_runner);
  }
  {
    auto p = scoped_prof("startup::exec_runtime::ee-start");
    ee_thread.start(ee_runner);
  }

  // step 4: wait for EE to signal a shutdown. meanwhile, run video loop on main thread.
  // TODO relegate this to its own function
  if (enable_display) {
    try {
      Gfx::Loop([]() { return MasterExit == RuntimeExitStatus::RUNNING; });
    } catch (std::exception& e) {
      lg::error("Exception thrown from graphics loop: {}", e.what());
      lg::error("Everything will crash now. good luck");
      throw;
    }
  }

  // hack to make the IOP die quicker if it's loading/unloading music
  gMusicFade = 0;

  // if we have no display, wait here for DECI to shutdown
  deci_thread.join();

  // fully shut down EE first before stopping the other threads
  ee_thread.join();

  // to be extra sure
  tm.shutdown();

  // join and exit
  tm.join();

  // kill renderer after all threads are stopped.
  // this makes sure the std::shared_ptr<Display> is destroyed in the main thread.
  if (enable_display) {
    Gfx::Exit();
  }
  lg::info("GOAL Runtime Shutdown (code {})", fmt::underlying(MasterExit));
#if defined(__3DS__)
  free(g_ee_main_mem);
#else
  munmap(g_ee_main_mem, EE_MAIN_MEM_SIZE);
#endif
  Discord_Shutdown();
  return MasterExit;
}
