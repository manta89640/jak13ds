# (AI-assisted)
# Minimal CMake toolchain for the Nintendo 3DS (devkitARM + libctru).
# Works with the official devkitPro packages (/opt/devkitpro) or the user-space bootstrap
# (platform/3ds/toolchain/bootstrap.sh, CTRULIB=~/devkitpro-3ds/libctru).
#
#   cmake -S platform/3ds -B build-3ds -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/platform/3ds/cmake/Toolchain-3DS.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(NINTENDO_3DS TRUE)

if(NOT DEFINED ENV{DEVKITPRO})
  set(DEVKITPRO /opt/devkitpro)
else()
  set(DEVKITPRO $ENV{DEVKITPRO})
endif()
if(NOT DEFINED ENV{DEVKITARM})
  set(DEVKITARM ${DEVKITPRO}/devkitARM)
else()
  set(DEVKITARM $ENV{DEVKITARM})
endif()

if(DEFINED ENV{CTRULIB})
  set(CTRULIB $ENV{CTRULIB})
elseif(EXISTS ${DEVKITPRO}/libctru/lib/libctru.a)
  set(CTRULIB ${DEVKITPRO}/libctru)
else()
  set(CTRULIB $ENV{HOME}/devkitpro-3ds/libctru)
endif()
if(NOT EXISTS ${CTRULIB}/lib/libctru.a)
  message(FATAL_ERROR "libctru not found (CTRULIB=${CTRULIB}). See docs/3ds-port/toolchain.md")
endif()

# 3dsxtool / smdhtool: official tools dir or the bootstrap prefix
find_program(3DSXTOOL 3dsxtool HINTS ${DEVKITPRO}/tools/bin ${CTRULIB}/../tools/bin)
find_program(SMDHTOOL smdhtool HINTS ${DEVKITPRO}/tools/bin ${CTRULIB}/../tools/bin)

set(CMAKE_C_COMPILER ${DEVKITARM}/bin/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITARM}/bin/arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${DEVKITARM}/bin/arm-none-eabi-gcc)
set(CMAKE_AR ${DEVKITARM}/bin/arm-none-eabi-gcc-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${DEVKITARM}/bin/arm-none-eabi-gcc-ranlib CACHE FILEPATH "")

set(ARCH_FLAGS "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft")
set(CMAKE_C_FLAGS_INIT "${ARCH_FLAGS} -D__3DS__ -ffunction-sections -fdata-sections -mword-relocations")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_ASM_FLAGS_INIT "${ARCH_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=3dsx.specs ${ARCH_FLAGS} -Wl,--gc-sections -L${CTRULIB}/lib")

include_directories(SYSTEM ${CTRULIB}/include)

set(CMAKE_FIND_ROOT_PATH ${DEVKITARM} ${DEVKITARM}/arm-none-eabi ${CTRULIB})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
