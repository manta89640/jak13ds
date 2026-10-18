# (AI-assisted)
# The 3DS renderer: game/graphics/ctr (renderer, GL-free) + the citro3d backend and its shader.

target_sources(og3ds_runtime PRIVATE
  ${OG_ROOT}/game/graphics/ctr/CtrButtonArt.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrDirect.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrEye.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrLevel.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrStream.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrImpostor.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrFineVis.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrOccCull.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrMerc.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrOcean.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrOceanFull.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrOceanNative.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrOceanVuMid.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrOceanVuNear.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrSettings.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrShadow.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrKeiraRig.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrSprite.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrSky.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrRenderer.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrVram.cpp
  ${OG_ROOT}/game/graphics/ctr/c3l_reader.cpp
)

# (AI-assisted) the decoders for compressed .c3l files (v11): LZ4 and zstd (decompression only)
set(OG3DS_ZSTD ${OG_ROOT}/third-party/zstd/lib)
add_library(og3ds_unpack STATIC
  ${OG_ROOT}/third-party/lz4/lz4.c
  ${OG3DS_ZSTD}/common/debug.c
  ${OG3DS_ZSTD}/common/entropy_common.c
  ${OG3DS_ZSTD}/common/error_private.c
  ${OG3DS_ZSTD}/common/fse_decompress.c
  ${OG3DS_ZSTD}/common/xxhash.c
  ${OG3DS_ZSTD}/common/zstd_common.c
  ${OG3DS_ZSTD}/decompress/huf_decompress.c
  ${OG3DS_ZSTD}/decompress/zstd_ddict.c
  ${OG3DS_ZSTD}/decompress/zstd_decompress.c
  ${OG3DS_ZSTD}/decompress/zstd_decompress_block.c
)
target_compile_definitions(og3ds_unpack PRIVATE ZSTD_LEGACY_SUPPORT=0 ZSTD_STRIP_ERROR_STRINGS)
# zstd 1.5.0 xxhash.c: u32 vs uint32_t pointers (an error since gcc 14)
target_compile_options(og3ds_unpack PRIVATE -Wno-incompatible-pointer-types)
target_link_libraries(og3ds_runtime og3ds_unpack)

find_program(PICASSO picasso HINTS ${DEVKITPRO}/tools/bin ${CTRULIB}/../tools/bin REQUIRED)

set(OG3DS_SHADER_CS "")
foreach(shader ctr_basic ctr_clip ctr_clip2 ctr_ocean ctr_ocean_full ctr_ocean_full_clip
    ctr_basic_old ctr_mesh_old ctr_skin_old ctr_skin_env_old ctr_bench_sep
    ctr_mesh_old_fa ctr_mesh_opt ctr_mesh_opt_fa ctr_skin_opt ctr_skin_env_opt
    ctr_ocean_full_opt ctr_ocean_full_clip_opt ctr_mesh_opt_df ctr_mesh_opt_df_fa)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.v.pica)
  set(bin ${CMAKE_CURRENT_BINARY_DIR}/${shader}.shbin)
  set(csrc ${CMAKE_CURRENT_BINARY_DIR}/${shader}_shbin.c)
  add_custom_command(
    OUTPUT ${csrc}
    COMMAND ${PICASSO} -o ${bin} ${src}
    COMMAND ${CMAKE_COMMAND} -DIN=${bin} -DOUT=${csrc} -DNAME=${shader}_shbin
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
    DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
    COMMENT "Assembling ${shader}.v.pica"
    VERBATIM)
  list(APPEND OG3DS_SHADER_CS ${csrc})
endforeach()

# (AI-assisted) config.ini pica_bench: one DVLB with an entry point per source file (picasso
# links them into one shbin with shared code), for the shader cost and program switch tests
set(OG3DS_BENCH_SHADERS ctr_bench_v7 ctr_bench_v7b ctr_bench_v14 ctr_bench_v21 ctr_bench_v35
    ctr_bench_v53 ctr_bench_o3 ctr_bench_o4 ctr_bench_o3p)
set(bench_srcs "")
foreach(shader ${OG3DS_BENCH_SHADERS})
  list(APPEND bench_srcs ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.v.pica)
endforeach()
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ctr_bench_shbin.c
  COMMAND ${PICASSO} -o ${CMAKE_CURRENT_BINARY_DIR}/ctr_bench.shbin ${bench_srcs}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_BINARY_DIR}/ctr_bench.shbin
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ctr_bench_shbin.c -DNAME=ctr_bench_shbin
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
  DEPENDS ${bench_srcs} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
  COMMENT "Assembling the pica_bench shaders"
  VERBATIM)
list(APPEND OG3DS_SHADER_CS ${CMAKE_CURRENT_BINARY_DIR}/ctr_bench_shbin.c)

target_sources(og3ds_port PRIVATE
  ${OG_ROOT}/game/graphics/ctr/ctr_gpu_citro3d.c
  ${OG_ROOT}/game/graphics/ctr/ctr_gpu_bench.c
  ${OG3DS_SHADER_CS}
)
target_include_directories(og3ds_port PRIVATE ${OG_ROOT})
