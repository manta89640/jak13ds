# (AI-assisted)
# Nintendo 3DS build of the OpenGOAL runtime (Jak 1 only, no OpenGL/SDL/imgui/discord/curl/
# cubeb/sockets). Standalone project so the host build is untouched:
#
#   source platform/3ds/toolchain/env.sh
#   cmake -S platform/3ds -B build-3ds -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/platform/3ds/cmake/Toolchain-3DS.cmake
#   cmake --build build-3ds
#
# Produces libgk3ds_*.a static libraries and (when everything links) gk.3dsx.

cmake_minimum_required(VERSION 3.20)
project(opengoal_3ds C CXX ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT NINTENDO_3DS)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=$PWD/platform/3ds/cmake/Toolchain-3DS.cmake")
endif()

get_filename_component(OG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")

# 32/48 MB GOAL memory layout (common/goal_constants.h). The GOAL code (C modules, CGO/DGO) must be
# built with the same setting: platform/3ds/tools/build_cmodules.sh
option(OG3DS_SMALL_MEMORY "Small (48 MB) EE memory layout" ON)
if(OG3DS_SMALL_MEMORY)
  add_compile_definitions(OPENGOAL_SMALL_MEMORY=1)
else()
  add_compile_definitions(OPENGOAL_SMALL_MEMORY=0)
endif()

add_compile_definitions(
  FMT_HEADER_ONLY=0
  OPENGOAL_3DS=1
  OPENGOAL_SCALAR_SIMD=1
  # SDL headers are included (types only) but SDL is never linked
  SDL_MAIN_HANDLED
)
# also set by the toolchain (CMAKE_<LANG>_FLAGS_INIT); repeated here so that a user-provided
# CMAKE_CXX_FLAGS cannot drop them
add_compile_definitions(__3DS__)
add_compile_options(
  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
  -ffunction-sections -fdata-sections -mword-relocations
  -Wno-psabi
  -fno-strict-aliasing
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>
)

include_directories(
  ${OG_ROOT}
  ${OG_ROOT}/third-party
  ${OG_ROOT}/third-party/fmt/include
  ${OG_ROOT}/third-party/json/single_include
  ${OG_ROOT}/third-party/SDL/include
  ${OG_ROOT}/third-party/zstd/lib
  ${OG_ROOT}/third-party/imgui
)

# ---------------- third party ----------------
add_library(og3ds_fmt STATIC ${OG_ROOT}/third-party/fmt/src/format.cc ${OG_ROOT}/third-party/fmt/src/os.cc)
add_library(og3ds_libco STATIC ${OG_ROOT}/third-party/libco/libco.c)

# ---------------- common ----------------
set(OG3DS_COMMON_SOURCES
  cross_os_debug/xdbg.cpp
  cross_sockets/XSocket.cpp
  cross_sockets/XSocketServer.cpp
  dma/dma_copy.cpp
  dma/dma.cpp
  dma/gs.cpp
  global_profiler/GlobalProfiler.cpp
  log/log.cpp
  util/Assert.cpp
  util/BitUtils.cpp
  util/crc32.cpp
  util/dgo_util.cpp
  util/FileUtil.cpp
  util/font/dbs/font_db_jak1.cpp
  util/font/dbs/font_db_jak2.cpp
  util/font/dbs/font_db_jak3.cpp
  util/font/font_utils_korean.cpp
  util/font/font_utils.cpp
  util/FrameLimiter.cpp
  util/json_util.cpp
  util/os.cpp
  util/print_float.cpp
  util/string_util.cpp
  util/Timer.cpp
  util/unicode_util.cpp
  versions/versions.cpp
)
list(TRANSFORM OG3DS_COMMON_SOURCES PREPEND ${OG_ROOT}/common/)
add_library(og3ds_common STATIC ${OG3DS_COMMON_SOURCES})
# FileUtil.cpp includes fpng.cpp, which needs the byte order
set_source_files_properties(${OG_ROOT}/common/util/FileUtil.cpp PROPERTIES COMPILE_DEFINITIONS "__BYTE_ORDER=1234")

# ---------------- sound (989snd with FakePlayer, no cubeb) ----------------
set(OG3DS_SOUND_SOURCES
  989snd/player.cpp
  989snd/midi_handler.cpp
  989snd/ame_handler.cpp
  989snd/blocksound_handler.cpp
  989snd/musicbank.cpp
  989snd/sfxblock.cpp
  989snd/sfxgrain.cpp
  989snd/plugin.cpp
  989snd/loader.cpp
  989snd/vagvoice.cpp
  989snd/lfo.cpp
  989snd/util.cpp
  common/synth.cpp
  common/voice.cpp
  common/envelope.cpp
  sndshim.cpp
  sdshim.cpp
)
list(TRANSFORM OG3DS_SOUND_SOURCES PREPEND ${OG_ROOT}/game/sound/)
add_library(og3ds_sound STATIC ${OG3DS_SOUND_SOURCES})

# ---------------- runtime: files owned by the runtime (kernel) work ----------------
set(OG3DS_KERNEL_SOURCES
  kernel/common/fileio.cpp
  kernel/common/goalc_context.cpp
  kernel/common/goalc_runtime.cpp
  kernel/common/kboot.cpp
  kernel/common/kdgo.cpp
  kernel/common/kdsnetm.cpp
  kernel/common/klink.cpp
  kernel/common/klisten.cpp
  kernel/common/kmachine.cpp
  kernel/common/kmalloc.cpp
  kernel/common/kperf.cpp
  kernel/common/kmemcard.cpp
  kernel/common/kprint.cpp
  kernel/common/kscheme.cpp
  kernel/common/ksocket.cpp
  kernel/common/ksound.cpp
  kernel/jak1/fileio.cpp
  kernel/jak1/goalc_kernel.cpp
  kernel/jak1/kboot.cpp
  kernel/jak1/kdgo.cpp
  kernel/jak1/klink.cpp
  kernel/jak1/klisten.cpp
  kernel/jak1/kmachine.cpp
  kernel/jak1/kprint.cpp
  kernel/jak1/kscheme.cpp
  kernel/jak1/ksound.cpp
  mips2c/mips2c_goalc.cpp
  mips2c/mips2c_table.cpp
  runtime.cpp
)
list(TRANSFORM OG3DS_KERNEL_SOURCES PREPEND ${OG_ROOT}/game/)
add_library(og3ds_kernel STATIC ${OG3DS_KERNEL_SOURCES})

# ---------------- runtime: everything else (platform-owned) ----------------
file(GLOB OG3DS_MIPS2C_JAK1 ${OG_ROOT}/game/mips2c/jak1_functions/*.cpp)
set(OG3DS_RUNTIME_SOURCES
  graphics/gfx.cpp
  graphics/display.cpp
  graphics/pipelines/null.cpp
  graphics/sceGraphicsInterface.cpp
  overlord/common/dma.cpp
  overlord/common/fake_iso.cpp
  overlord/common/iso_api.cpp
  overlord/common/iso.cpp
  overlord/common/isocommon.cpp
  overlord/common/overlord.cpp
  overlord/common/sbank.cpp
  overlord/common/soundcommon.cpp
  overlord/common/srpc.cpp
  overlord/common/ssound.cpp
  overlord/jak1/dma.cpp
  overlord/jak1/fake_iso.cpp
  overlord/jak1/iso_api.cpp
  overlord/jak1/iso_queue.cpp
  overlord/jak1/iso.cpp
  overlord/jak1/isocommon.cpp
  overlord/jak1/overlord.cpp
  overlord/jak1/ramdisk.cpp
  overlord/jak1/srpc.cpp
  overlord/jak1/ssound.cpp
  overlord/jak1/stream.cpp
  sce/deci2.cpp
  sce/iop.cpp
  sce/libcdvd_ee.cpp
  sce/libdma.cpp
  sce/libgraph.cpp
  sce/libpad.cpp
  sce/pad_script.cpp
  sce/libscf.cpp
  sce/sif_ee_memcard.cpp
  sce/sif_ee.cpp
  sce/stubs.cpp
  settings/settings.cpp
  system/background_worker.cpp
  system/Deci2Server.cpp
  system/hid/hid_null.cpp
  system/IOP_Kernel.cpp
  system/iop_thread.cpp
  system/SystemThread.cpp
  external/discord.cpp
  external/discord_jak1.cpp
  external/discord_jak2.cpp
  external/discord_jak3.cpp
  external/discord_jakx.cpp
  external/discord_rpc_null.cpp
  graphics/screenshot.cpp
  system/hid/input_bindings.cpp
  tools/filter_menu/filter_menu_json.cpp
)
list(TRANSFORM OG3DS_RUNTIME_SOURCES PREPEND ${OG_ROOT}/game/)
add_library(og3ds_runtime STATIC ${OG3DS_RUNTIME_SOURCES} ${OG3DS_MIPS2C_JAK1})

# ---------------- gk.3dsx ----------------
add_library(og3ds_port STATIC ${CMAKE_CURRENT_SOURCE_DIR}/port/ctr_port.c)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ctr_renderer.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmodules.cmake)

add_executable(gk ${CMAKE_CURRENT_SOURCE_DIR}/gk/main.cpp ${OG3DS_REGISTRY_OBJ})
target_link_libraries(gk
  -Wl,--start-group
  og3ds_kernel og3ds_runtime og3ds_sound og3ds_common og3ds_fmt og3ds_libco og3ds_port
  ${OG3DS_GOAL_MODULES_LIB}
  -Wl,--end-group
  citro3d ctru m)
set_target_properties(gk PROPERTIES SUFFIX ".elf")

if(3DSXTOOL AND SMDHTOOL)
  add_custom_command(TARGET gk POST_BUILD
    COMMAND ${SMDHTOOL} --create "OpenGOAL" "Jak and Daxter (OpenGOAL 3DS port)" "OpenGOAL"
            ${CTRULIB}/default_icon.png ${CMAKE_CURRENT_BINARY_DIR}/gk.smdh
    COMMAND ${3DSXTOOL} $<TARGET_FILE:gk> ${CMAKE_CURRENT_BINARY_DIR}/gk.3dsx
            --smdh=${CMAKE_CURRENT_BINARY_DIR}/gk.smdh
    COMMENT "Building gk.3dsx"
    VERBATIM)
endif()
