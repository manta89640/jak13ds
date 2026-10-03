add_subdirectory(level_tools)

add_executable(dgo_unpacker
        dgo_unpacker.cpp)
target_link_libraries(dgo_unpacker common)

add_executable(dgo_packer
        dgo_packer.cpp)
target_link_libraries(dgo_packer common)

add_executable(memory_dump_tool
        memory_dump_tool/main.cpp)
target_link_libraries(memory_dump_tool common decomp)

add_executable(type_searcher
        type_searcher/main.cpp)
target_link_libraries(type_searcher common decomp)

add_executable(formatter
        formatter/main.cpp)
target_link_libraries(formatter common tree-sitter)

# (AI-assisted) .fr3 -> .c3l converter for the 3DS renderer (docs/3ds-port/c3l_format.md)
# (AI-assisted) lz4 / zstd: compressed c3l blocks (v11); meshoptimizer: vertex cache order
set(CTR_LEVEL_CONVERTER_THIRD_PARTY
        ${CMAKE_SOURCE_DIR}/third-party/rg_etc1/rg_etc1.cpp
        ${CMAKE_SOURCE_DIR}/third-party/lz4/lz4.c
        ${CMAKE_SOURCE_DIR}/third-party/lz4/lz4hc.c
        ${CMAKE_SOURCE_DIR}/third-party/meshoptimizer/allocator.cpp
        ${CMAKE_SOURCE_DIR}/third-party/meshoptimizer/indexanalyzer.cpp
        ${CMAKE_SOURCE_DIR}/third-party/meshoptimizer/vcacheoptimizer.cpp
        ${CMAKE_SOURCE_DIR}/third-party/meshoptimizer/vfetchoptimizer.cpp)
add_executable(ctr_level_converter
        ctr_level_converter/main.cpp
        ctr_level_converter/ctr_texture.cpp
        ctr_level_converter/c3l_pack.cpp
        ${CMAKE_SOURCE_DIR}/game/graphics/ctr/c3l_reader.cpp
        ${CTR_LEVEL_CONVERTER_THIRD_PARTY})
# third-party code: not our warnings
set_source_files_properties(${CTR_LEVEL_CONVERTER_THIRD_PARTY} PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(ctr_level_converter common)
