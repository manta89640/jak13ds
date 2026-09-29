# (AI-assisted)
# The 3DS renderer: game/graphics/ctr (renderer, GL-free) + the citro3d backend and its shader.

target_sources(og3ds_runtime PRIVATE
  ${OG_ROOT}/game/graphics/ctr/CtrDirect.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrRenderer.cpp
  ${OG_ROOT}/game/graphics/ctr/CtrVram.cpp
)

find_program(PICASSO picasso HINTS ${DEVKITPRO}/tools/bin ${CTRULIB}/../tools/bin REQUIRED)

set(OG3DS_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ctr_basic.v.pica)
set(OG3DS_SHADER_BIN ${CMAKE_CURRENT_BINARY_DIR}/ctr_basic.shbin)
set(OG3DS_SHADER_C ${CMAKE_CURRENT_BINARY_DIR}/ctr_basic_shbin.c)
add_custom_command(
  OUTPUT ${OG3DS_SHADER_C}
  COMMAND ${PICASSO} -o ${OG3DS_SHADER_BIN} ${OG3DS_SHADER_SRC}
  COMMAND ${CMAKE_COMMAND} -DIN=${OG3DS_SHADER_BIN} -DOUT=${OG3DS_SHADER_C} -DNAME=ctr_basic_shbin
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
  DEPENDS ${OG3DS_SHADER_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
  COMMENT "Assembling ctr_basic.v.pica"
  VERBATIM)

target_sources(og3ds_port PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/port/ctr_gpu_citro3d.c
  ${OG3DS_SHADER_C}
)
target_include_directories(og3ds_port PRIVATE ${OG_ROOT})
