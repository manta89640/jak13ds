#!/usr/bin/env bash
# (AI-assisted)
# User-space 3DS toolchain bootstrap (no sudo).
#
# Requires devkitARM already present (default /opt/devkitpro/devkitARM, installed by the
# devkitPro pacman installer). Builds libctru + citro3d + 3dstools + picasso from the official
# devkitPro GitHub sources into $DKP3DS (default ~/devkitpro-3ds). This avoids `sudo dkp-pacman`,
# which is the only supported way to install the prebuilt 3DS packages into /opt/devkitpro.
#
# Host deps (Homebrew): autoconf automake libtool (for 3dstools/picasso).
#
# Afterwards: source platform/3ds/toolchain/env.sh
set -euo pipefail

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
DKP3DS="${DKP3DS:-$HOME/devkitpro-3ds}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

LIBCTRU_TAG=v2.7.0
CITRO3D_TAG=v1.7.1
TOOLS3DS_TAG=v1.3.1
PICASSO_TAG=v2.7.2

if [ ! -x "$DEVKITARM/bin/arm-none-eabi-gcc" ]; then
  echo "devkitARM not found at $DEVKITARM. See docs/3ds-port/toolchain.md." >&2
  exit 1
fi

export DEVKITPRO DEVKITARM
export PATH="$DKP3DS/tools/bin:$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

mkdir -p "$DKP3DS/src"
cd "$DKP3DS/src"

fetch() { # repo tag
  if [ ! -d "$1" ]; then git clone -q "https://github.com/devkitPro/$1.git"; fi
  git -C "$1" fetch -q --tags
  git -C "$1" -c advice.detachedHead=false checkout -q "$2"
}

fetch 3dstools "$TOOLS3DS_TAG"
fetch picasso "$PICASSO_TAG"
fetch libctru "$LIBCTRU_TAG"
fetch citro3d "$CITRO3D_TAG"

for t in 3dstools picasso; do
  (cd "$t" && ./autogen.sh && ./configure --prefix="$DKP3DS/tools" && make -j"$JOBS" && make install)
done

# libctru: same layout as the official package ($CTRULIB/{include,lib,default_icon.png})
(cd libctru/libctru && make -j"$JOBS")
rm -rf "$DKP3DS/libctru"
mkdir -p "$DKP3DS/libctru"
cp -r libctru/libctru/include libctru/libctru/lib libctru/libctru/default_icon.png "$DKP3DS/libctru/"

# citro3d: the official package installs into $DEVKITPRO/libctru too
(cd citro3d && make clean && make -j"$JOBS" CTRULIB="$DKP3DS/libctru")
cp -r citro3d/include/* "$DKP3DS/libctru/include/"
cp citro3d/lib/* "$DKP3DS/libctru/lib/"

echo "3DS toolchain ready in $DKP3DS. Use: source platform/3ds/toolchain/env.sh"
