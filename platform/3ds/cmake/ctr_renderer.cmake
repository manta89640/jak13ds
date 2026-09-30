# (AI-assisted)
# The 3DS renderer: game/graphics/ctr (renderer, GL-free) + the citro3d backend and its shader.

target_sources(og3ds_runtime PRIVATE
  ${OG_ROOT}/game/graphics/ctr/CtrDirect.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrLevel.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrMerc.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrSprite.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrRenderer.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrVram.cpp
)

find_program(PICASSO picasso HINTS ${DEVKITPRO}/tools/bin ${CTRULIB}/../tools/bin REQUIRED)

set(OG3DS_SHADER_CS "")
foreach(shader ctr_basic ctr_mesh ctr_skin)
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

target_sources(og3ds_port PRIVATE
  ${OG_ROOT}/game/graphics/ctr/ctr_gpu_citro3d.c
  ${OG3DS_SHADER_CS}
)
target_include_directories(og3ds_port PRIVATE ${OG_ROOT})
