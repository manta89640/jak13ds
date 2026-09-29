# (AI-assisted)
# GOAL code compiled to C (goalc --instruction-set c), linked statically into gk.3dsx.
#
# OG3DS_CSRC_DIR: the out/jak1/csrc folder of the project the game was built from. The .CGO/.DGO
# files staged on the SD card must come from the same build (modules are matched by hash).
# Empty = no GOAL modules (the kernel then can't load anything compiled for C).
#
# Rebuild everything after a GOAL change: platform/3ds/tools/build_cmodules.sh

set(OG3DS_CSRC_DIR "" CACHE PATH "out/<game>/csrc folder with the C backend output to link into gk")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OG3DS_REGISTRY_OBJ "")
set(OG3DS_GOAL_MODULES_LIB "")
if(OG3DS_CSRC_DIR)
  file(GLOB OG3DS_CSRC_FILES CONFIGURE_DEPENDS ${OG3DS_CSRC_DIR}/*.c)
  list(LENGTH OG3DS_CSRC_FILES OG3DS_CSRC_COUNT)
  message(STATUS "GOAL C modules: ${OG3DS_CSRC_COUNT} files from ${OG3DS_CSRC_DIR}")
  if(OG3DS_CSRC_COUNT EQUAL 0)
    message(FATAL_ERROR "OG3DS_CSRC_DIR=${OG3DS_CSRC_DIR} has no .c files")
  endif()

  add_library(og3ds_goal_modules STATIC ${OG3DS_CSRC_FILES})
  target_compile_definitions(og3ds_goal_modules PRIVATE GOALC_STATIC)
  target_include_directories(og3ds_goal_modules PRIVATE
    ${OG_ROOT}/goalc/cbackend ${OG_ROOT}/game/kernel/common)
  target_compile_options(og3ds_goal_modules PRIVATE -O2 -fno-strict-aliasing -w)

  # registry: compiled into gk itself (an object, not an archive member), so it replaces the
  # runtime's weak goalc_register_static_modules() and pulls in every module.
  set(OG3DS_REGISTRY_C ${CMAKE_CURRENT_BINARY_DIR}/goalc_static_registry.c)
  add_custom_command(
    OUTPUT ${OG3DS_REGISTRY_C}
    COMMAND ${Python3_EXECUTABLE} ${OG_ROOT}/scripts/3ds/gen_c_registry.py
            ${OG3DS_CSRC_DIR} ${OG3DS_REGISTRY_C}
    DEPENDS ${OG3DS_CSRC_FILES} ${OG_ROOT}/scripts/3ds/gen_c_registry.py
    COMMENT "Generating the GOAL C module registry"
    VERBATIM)
  set_source_files_properties(${OG3DS_REGISTRY_C} PROPERTIES
    INCLUDE_DIRECTORIES "${OG_ROOT}/game/kernel/common")
  set(OG3DS_REGISTRY_OBJ ${OG3DS_REGISTRY_C})
  set(OG3DS_GOAL_MODULES_LIB og3ds_goal_modules)
endif()
