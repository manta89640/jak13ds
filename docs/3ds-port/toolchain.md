# 3DS toolchain (macOS, Apple Silicon)

(AI-assisted.)

## Current state on this machine

- `devkitARM` r67.1 (GCC 15.2.0) is installed in `/opt/devkitpro` (root-owned, installed earlier
  by the devkitPro pacman installer together with the GBA packages).
- The 3DS packages (`libctru`, `citro3d`, `3dstools`, `picasso`, ...) were **not** installed there.
  Installing them with `dkp-pacman` needs root, and pkg.devkitpro.org does not serve packages to
  plain `curl`, so instead they are built from the official devkitPro GitHub sources into a
  user-space prefix (no sudo):

  ```sh
  brew install autoconf automake libtool      # host deps for 3dstools / picasso
  platform/3ds/toolchain/bootstrap.sh          # -> ~/devkitpro-3ds (override with DKP3DS=...)
  ```

  Result in `~/devkitpro-3ds`:
  - `libctru/{include,lib}`: libctru v2.7.0 + citro3d v1.7.1 (same layout as the official
    package; `CTRULIB` points here)
  - `tools/bin`: `3dsxtool`, `smdhtool`, `3dsxdump`, `mkromfs3ds` (3dstools v1.3.1), `picasso`
  - Not built: `tex3ds` (needs ImageMagick), `citro2d`, `3dslink`, `makerom`/`bannertool` (CIA).

- `source platform/3ds/toolchain/env.sh` sets `DEVKITPRO`, `DEVKITARM`, `CTRULIB`, `PATH`.
  It prefers the official install in `/opt/devkitpro/libctru` if it exists.

## Preferred: official packages (needs sudo once)

If you are fine with sudo, this is the supported setup and replaces the user-space bootstrap:

```sh
sudo /opt/devkitpro/pacman/bin/pacman -Sy
sudo /opt/devkitpro/pacman/bin/pacman -S 3ds-dev          # libctru, citro3d, citro2d, 3dstools,
                                                          # picasso, tex3ds, 3ds-cmake, 3dslink, examples
# optional
sudo /opt/devkitpro/pacman/bin/pacman -S 3ds-portlibs     # zlib, libpng, ... for 3DS
```

(`dkp-pacman` is the same binary; on macOS it lives at `/opt/devkitpro/pacman/bin/pacman`. If it
complains about GPGME, put `/opt/devkitpro/pacman/bin` first in `PATH`.)

After that `env.sh` picks up `/opt/devkitpro/libctru` automatically, and the devkitPro CMake
toolchain `/opt/devkitpro/cmake/3DS.cmake` (from `3ds-cmake`) becomes available.

## Hello world

```sh
source platform/3ds/toolchain/env.sh
make -C platform/3ds/hello        # -> platform/3ds/hello/opengoal-hello.3dsx
```

The program clears the top screen with citro3d (pulsing orange) and prints `sizeof(void*)`,
free linear/application memory, and a frame counter on the bottom screen. START exits.

## Emulator

No Homebrew cask exists for Azahar / Lime3DS / Citra. Azahar publishes a macOS arm64 build on
GitHub that runs from a user directory (no sudo):

```sh
mkdir -p ~/devkitpro-3ds/emu && cd ~/devkitpro-3ds/emu
curl -LO https://github.com/azahar-emu/azahar/releases/download/2126.1.2/azahar-macos-arm64-2126.1.2.zip
unzip azahar-macos-arm64-2126.1.2.zip
xattr -dr com.apple.quarantine azahar-macos-arm64-2126.1.2/Azahar.app
azahar-macos-arm64-2126.1.2/Azahar.app/Contents/MacOS/azahar -w \
    platform/3ds/hello/opengoal-hello.3dsx
```

This is installed in `~/devkitpro-3ds/emu`. Azahar has no headless mode (options: `-w`, `-f`,
`-d <video file>`, `-g <gdb port>`). Launched from the agent session the process started and
logged its service init, but no window/frames could be observed (no GUI access from the sandbox),
so booting the hello app is **not verified yet**: run the command above from a normal terminal.
For automated tests later: `-g 24689` (GDB stub) plus `arm-none-eabi-gdb` can check that the app
reaches a breakpoint, and `-d out.mp4` dumps video.
