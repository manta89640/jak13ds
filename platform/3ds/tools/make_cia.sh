#!/usr/bin/env bash
# (AI-assisted)
# Package gk.elf as an installable CIA (New 3DS 124 MB memory mode). See docs/3ds-port/3ds_build.md.
#
#   platform/3ds/tools/make_cia.sh [--build-dir DIR] [--out FILE] [--boottest] [--mem 124|legacy]
#                                  [--game jak1|jak2]
#
# --game jak2: (AI-assisted) the Jak II build (build-3ds-jak2/gk.elf -> jak2.cia): its own title
#   ("OpenGOAL Jak II", unique id 0xF7A21, product code CTR-H-OGJ2; gk.rsf's settings otherwise)
#   and its art from platform/3ds/cia/local/jak2/ (banner.png, banner.wav, icon.png).
#
# --boottest: package the small test app platform/3ds/hello (build it first: make -C
#   platform/3ds/hello) as four separate titles, each with one CIA setting changed, to find the
#   setting that keeps a title from starting (the game's CIA stays on the launch screen):
#     boottest_124.cia        the game's settings (124 MB mode, 804 MHz, L2 cache)
#     boottest_legacy.cia     without the 124 MB mode
#     boottest_plain.cia      like most homebrew CIAs (FBI): no 124 MB mode, 268 MHz, no L2 cache
#     boottest_nocompress.cia the game's settings with the code not compressed
#   Each one appends lines (with its title id) to sdmc:/boottest.txt and
#   sdmc:/3ds/jak1/boottest.txt and shows the same on its bottom screen.
# --mem legacy: without the New 3DS 124 MB memory mode (the game then says there isn't enough
#   memory, but it shows whether that mode is what keeps the title from starting).
#
# Needs makerom in PATH or in ~/devkitpro-3ds/tools/bin (https://github.com/3DSGuy/Project_CTR
# releases). The banner (platform/3ds/cia/banner.png + banner.wav, the title screen) and the icon
# (cia/icon.png) are made with bannertool, from PATH or ~/devkitpro-3ds/tools/bin. There is no
# macOS release: build it from https://github.com/diasurgical/bannertool with
#   clang -c source/pc/stb_image.c source/pc/stb_vorbis.c
#   clang++ -std=c++14 -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=0 -Isource \
#     source/*.cpp source/pc/wav.cpp source/3ds/*.cpp stb_image.o stb_vorbis.o -o bannertool
# Without bannertool the CIA has no banner and the icon of gk.smdh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="$ROOT/build-3ds"
OUT=""
BOOTTEST=0
MEM=124
GAME=jak1
BUILD_DIR_GIVEN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; BUILD_DIR_GIVEN=1; shift 2 ;;
    --game) GAME="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --boottest) BOOTTEST=1; shift ;;
    --mem) MEM="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
case "$MEM" in 124|legacy) ;; *) echo "--mem: 124 or legacy" >&2; exit 1 ;; esac
case "$GAME" in jak1|jak2) ;; *) echo "--game: jak1 or jak2" >&2; exit 1 ;; esac
if [ "$GAME" = jak2 ] && [ "$BUILD_DIR_GIVEN" = 0 ]; then BUILD_DIR="$ROOT/build-3ds-jak2"; fi
MAKEROM="$(command -v makerom || true)"
[ -n "$MAKEROM" ] || MAKEROM="$HOME/devkitpro-3ds/tools/bin/makerom"
[ -x "$MAKEROM" ] || { echo "makerom not found (see the header of this script)" >&2; exit 1; }
echo "makerom: $MAKEROM ($("$MAKEROM" 2>&1 | head -1 || true))"

ELF="$BUILD_DIR/gk.elf"
SMDH="$BUILD_DIR/gk.smdh"
RSF="$ROOT/platform/3ds/cia/gk.rsf"
# (AI-assisted) shader output rule proven on hardware (a shader that left output components
# unwritten hung the real GPU, 2026-10-03)
python3 "$ROOT/platform/3ds/tools/check_shaders.py" "$ROOT/platform/3ds/shaders" ||
  { echo "make_cia: shader check failed (platform/3ds/tools/check_shaders.py)" >&2; exit 1; }
if [ "$BOOTTEST" = 1 ]; then
  ELF="$ROOT/platform/3ds/hello/opengoal-hello.elf"
  SMDH="$ROOT/platform/3ds/hello/opengoal-hello.smdh"
  RSF="$ROOT/platform/3ds/cia/boottest.rsf"
  [ -f "$ELF" ] || { echo "no $ELF (make -C platform/3ds/hello first)" >&2; exit 1; }
fi
[ -f "$ELF" ] || { echo "no $ELF (build gk first)" >&2; exit 1; }
[ -f "$SMDH" ] || { echo "no $SMDH" >&2; exit 1; }
# (AI-assisted) every system call in the ELF must be in the RSF's SystemCallAccess list: on
# hardware a missing one raises an exception (2026-10-03), Azahar ignores the list
python3 "$ROOT/platform/3ds/tools/check_svc.py" "$ELF" "$RSF" ||
  { echo "make_cia: svc check failed (platform/3ds/tools/check_svc.py)" >&2; exit 1; }
mkdir -p "$BUILD_DIR"

CIA_DIR="$ROOT/platform/3ds/cia"
LOCAL_ART="$CIA_DIR/local"
SHORT_TITLE="Jak and Daxter"
LONG_TITLE="The Precursor Legacy (OpenGOAL 3DS port)"
if [ "$GAME" = jak2 ]; then
  # (AI-assisted) Jak II: its own title and art; the rest of the settings are gk.rsf's
  LOCAL_ART="$CIA_DIR/local/jak2"
  SHORT_TITLE="OpenGOAL Jak II"
  LONG_TITLE="Jak II (OpenGOAL 3DS port)"
  mkdir -p "$BUILD_DIR"
  sed -e 's/^\(  Title *: *\).*/\1"OpenGOAL Jak II"/' \
      -e 's/^\(  ProductCode *: *\).*/\1"CTR-H-OGJ2"/' \
      -e 's/^\(  UniqueId *: *\).*/\10xF7A21/' "$RSF" > "$BUILD_DIR/gk_jak2.rsf"
  RSF="$BUILD_DIR/gk_jak2.rsf"
fi
# (AI-assisted) a banner made from your own copy of the game (the title logo, the power cell jingle):
# platform/3ds/cia/local/banner.png (256x128) and banner.wav (3 s at most). The folder is ignored
# by git: game assets are never committed. See docs/3ds-port/3ds_build.md, "Custom HOME Menu banner".
BANNER_PNG="$CIA_DIR/banner.png"
BANNER_WAV="$CIA_DIR/banner.wav"
[ -f "$LOCAL_ART/banner.png" ] && BANNER_PNG="$LOCAL_ART/banner.png"
[ -f "$LOCAL_ART/banner.wav" ] && BANNER_WAV="$LOCAL_ART/banner.wav"
# (AI-assisted) and a HOME Menu icon from a picture of your choice (make_banner.py --icon)
ICON_PNG="$CIA_DIR/icon.png"
[ -f "$LOCAL_ART/icon.png" ] && ICON_PNG="$LOCAL_ART/icon.png"
echo "banner: $BANNER_PNG, $BANNER_WAV; icon: $ICON_PNG"
BANNERTOOL="$(command -v bannertool || true)"
[ -n "$BANNERTOOL" ] || BANNERTOOL="$HOME/devkitpro-3ds/tools/bin/bannertool"
[ -x "$BANNERTOOL" ] || echo "bannertool not found: no banner (see the header of this script)" >&2

# makerom wants a stripped elf
STRIPPED="$BUILD_DIR/$(basename "$ELF" .elf)_cia.elf"
"${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-strip" -o "$STRIPPED" "$ELF"

# build_cia OUT RSF SHORT_TITLE LONG_TITLE
build_cia() {
  local out="$1" rsf="$2" short="$3" long="$4"
  local smdh="$SMDH" banner_args=()
  if [ -x "$BANNERTOOL" ]; then
    "$BANNERTOOL" makebanner -i "$BANNER_PNG" -a "$BANNER_WAV" \
      -o "$BUILD_DIR/banner.bnr" > /dev/null
    smdh="$BUILD_DIR/$(basename "$out" .cia).smdh"
    "$BANNERTOOL" makesmdh -s "$short" -l "$long" -p "OpenGOAL" -i "$ICON_PNG" \
      -o "$smdh" > /dev/null
    banner_args=(-banner "$BUILD_DIR/banner.bnr")
  fi
  "$MAKEROM" -f cia -o "$out" -elf "$STRIPPED" -rsf "$rsf" \
    -icon "$smdh" ${banner_args[@]+"${banner_args[@]}"} -exefslogo -target t -ver 0
  echo "cia: $out"
}

# variant RSF: variant_rsf OUT_RSF UNIQUE_ID SED_EXPRESSIONS...
variant_rsf() {
  local out="$1" uid="$2"
  shift 2
  sed -e "s/^\(  UniqueId *: *\).*/\1$uid/" "$@" "$RSF" > "$out"
}
LEGACY=(-e 's/^\(  SystemModeExt *: *\).*/\1Legacy/')

if [ "$BOOTTEST" = 1 ]; then
  [ -z "$OUT" ] || echo "--out is ignored with --boottest" >&2
  variant_rsf "$BUILD_DIR/bt_124.rsf" 0xF7A12
  build_cia "$BUILD_DIR/boottest_124.cia" "$BUILD_DIR/bt_124.rsf" "Jak1 boot test 124" \
    "CIA boot test: 124 MB, 804 MHz, L2"
  variant_rsf "$BUILD_DIR/bt_legacy.rsf" 0xF7A13 "${LEGACY[@]}"
  build_cia "$BUILD_DIR/boottest_legacy.cia" "$BUILD_DIR/bt_legacy.rsf" "Jak1 boot test legacy" \
    "CIA boot test: legacy memory, 804 MHz, L2"
  variant_rsf "$BUILD_DIR/bt_plain.rsf" 0xF7A14 "${LEGACY[@]}" \
    -e 's/^\(  CpuSpeed *: *\).*/\1268MHz/' -e 's/^\(  EnableL2Cache *: *\).*/\1false/'
  build_cia "$BUILD_DIR/boottest_plain.cia" "$BUILD_DIR/bt_plain.rsf" "Jak1 boot test plain" \
    "CIA boot test: legacy memory, 268 MHz, no L2"
  variant_rsf "$BUILD_DIR/bt_nocompress.rsf" 0xF7A15 \
    -e 's/^\(  EnableCompress *: *\).*/\1false/'
  build_cia "$BUILD_DIR/boottest_nocompress.cia" "$BUILD_DIR/bt_nocompress.rsf" \
    "Jak1 boot test nocomp" "CIA boot test: 124 MB, code not compressed"
  exit 0
fi

if [ -z "$OUT" ]; then
  OUT="$BUILD_DIR/$GAME.cia"
  [ "$MEM" = legacy ] && OUT="$BUILD_DIR/${GAME}_legacy.cia"
fi
if [ "$MEM" = legacy ]; then
  # the same settings without the 124 MB mode
  sed "${LEGACY[@]}" "$RSF" > "$BUILD_DIR/cia_legacy.rsf"
  RSF="$BUILD_DIR/cia_legacy.rsf"
fi
build_cia "$OUT" "$RSF" "$SHORT_TITLE" "$LONG_TITLE"
