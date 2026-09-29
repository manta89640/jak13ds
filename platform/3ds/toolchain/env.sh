# (AI-assisted)
# Source this to build 3DS code. Works both with the official packages (everything in
# /opt/devkitpro) and with the user-space bootstrap (platform/3ds/toolchain/bootstrap.sh).
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export DKP3DS="${DKP3DS:-$HOME/devkitpro-3ds}"
if [ -f "$DEVKITPRO/libctru/lib/libctru.a" ]; then
  export CTRULIB="$DEVKITPRO/libctru"
elif [ -f "$DKP3DS/libctru/lib/libctru.a" ]; then
  export CTRULIB="$DKP3DS/libctru"
  export PATH="$DKP3DS/tools/bin:$PATH"
else
  echo "libctru not found; run platform/3ds/toolchain/bootstrap.sh or see docs/3ds-port/toolchain.md" >&2
fi
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
