# Jak II -> New 3DS: status and handoff (baseline D, updated Oct 22 2026)

(AI-assisted.) Branch `jak2-3ds` (worktree `jak-project/.claude/worktrees/jak2`), started from
`batch-oct11` c1da64bc1; batch-oct11 9da503ce27 merged in on Oct 18 (no conflicts). Jak 1
behaviour is unchanged: every shared-code change is either Jak 2 only, gated on the game version
or an environment variable, or a hook that stays null for Jak 1.

## Where it stands

| Step | State |
|---|---|
| Disc | Verified: SCUS-97265, `SYSTEM.CNF` VER=2.01, NTSC; the extractor's hash check passed |
| Extract / decompile data | Done. `iso_data/jak2` 4.1 GB, `decompiler_out/jak2` 509 MB, 147 fr3 levels |
| Native PC build (arm64) | Done. Boots to the title and renders Haven City (slums, HUD minimap); silent |
| Native `(mi)` | 2273 targets build |
| C-backend compile of all of Jak 2 | **Strict build (no LENIENT): 840 modules, 2273 targets, ~2 min**; 310 warnings, reviewed below |
| C-mode boot on the Mac | **Works (Oct 18).** Title screen, 5 min idle stable at 30 fps; Haven City slums with traffic (291 processes) |
| C mode vs native | Same Jak position/state after the city pad; OpenGL screenshots of the title and the slums match native |
| ABI check run / stack poison run | Done, nothing real left (see "C mode on the Mac") |
| Haven City timing (the feasibility number) | Profiled (Oct 18, jak2_perf.md): Mac 0.44-0.70 ms/frame by district (~4.5x Jak 1); N3DS estimate ~77 ms city average, ~97 ms slums, unmodified; ~22 ms after natives, codegen and core-2 jobs |
| Cheap logic fixes (j2-cheap, Oct 18) | Azahar slums logic 75.8 -> 66.4 ms (13.2 -> 15.0 fps): entity-by-name and subtitle scene caches, no LED update, pc-prof gated, direct kernel entries; IOP on core 2 hung the boot and is reverted (jak2_perf.md "Done") |
| Scripted route into city gameplay | Works (jak2-route): `platform/3ds/tests/jak2_city.pad`, `jak2_route.pad` |
| Natives (j2-nat-port, j2-nat-new) | Merged; always on (Oct 18: the `logic_native` key and the other native switches were removed, config_dev.md) |
| Renderer (j2-render-bg, j2-render-fg) | Merged: backgrounds, merc, sky + clouds, particles, shadows, HUD, text, minimap |
| **Integration build, baseline B (Oct 18)** | **Azahar slums 29.8 fps (30 Hz cap): logic 25-27 ms, render thread 28.5-29.3 ms, GPU busy 29-30 ms; staged in `port_test/3ds-sdcard-jak2-baseline-b`** (section below) |
| Sound (j2-sound, Oct 18) | **Plays in Azahar** (muted runs): title stream, city music + banks, dialogue streams; 0 DSP drops, 0 late buffers. Mixer on core 1 (40% share): 12-14% of real time, 3.8-6.3 ms/frame there; slums 29.9 fps, render thread 27.7-28.2 ms (on core 2 it was 34 ms, 25-29 fps). 1.96 MB of banks in the slums. Compressed SD files (`--compress --compress-vag`) work for Jak II. Hardware not run. See 3ds_build.md "Jak II sound" |
| Texture animations, ocean, minimap (j2-texanim); memory (j2-mem) | Merged Oct 18: CPU texture animations, flat ocean (ocean.gc), opaque minimap; dormant levels, 1.5 MB stream arena |
| **Integration build, baseline C (Oct 18)** | **Azahar slums 29.4-29.6 fps with sound on: logic 21.5-22.5 ms, render thread 27-28 ms, GPU busy 28-29 ms, mixer 10-19% of real time, 0 DSP drops; linear free 2.9 MB, heap ~3.3 MB. Title intro stopped early (no JAK II logo): fixed after C, see "Title intro on the 3DS". Staged in `port_test/3ds-sdcard-jak2-baseline-c`** (section below) |
| Title intro (j2-title2, Oct 18) | **Fixed:** the intro (slums fly-through, Sony / Naughty Dog cards, JAK II logo from ~frame 1050) plays in Azahar as on the host; the cause was the 3DS mixer racing the overlord's SPU voice writes (section "Title intro on the 3DS") |
| Memory cuts (j2-mem2, Oct 18) | Sparse VRAM copy, no Jak 1 code, impostors off, no RAMDISK, smaller stacks, 1.25 MB command buffer, linear 34 MB: route heap free 6.5-9.0 MB (was 2.5-3.5), linear free >= 3.4 MB except the market with ctyindb in view (1.8 MB, was 128 KB); section "Memory: heap and linear cuts" |
| **Integration build, baseline D (Oct 22)** | **j2-title2 + j2-mem2 merged (144c36070b). Azahar: intro to the JAK II logo, title 29.1-29.7 fps, slums 29.5-29.6 fps (logic 22.5-23.3 ms), all route stops 29.6-29.7 fps; heap free 6.3-10.1 MB; 0 DSP drops. Staged in `port_test/3ds-sdcard-jak2-baseline-d`** (section below) |
| Title intro's first frames (j2intro, Oct 24) | **Fixed:** the slums fly-through could start before ctysluma's background was read (ctywide's walls over a black void for ~2 s in baseline D's Azahar run). Jak II's level login now waits for the level's .c3l (as Jak 1), the .c3l is read from the DGO's start, a black hold covers what's late at the reveal; the title's loads went from ~20 s to ~9 s (section "Title intro: missing geometry") |

## Integration build: baseline B (Oct 18)

(AI-assisted.) Everything merged into jak2-3ds (C mode, small memory, module instances, natives,
j2-cheap, j2-render-bg, j2-render-fg), then:
- Switches: `logic_native` (and Jak 1's `logic_micro`, `bones_native`, `collide_native`,
  `collide_frag_cache`, `anim_native`) removed; the natives always run (config_dev.md "Removed on
  Oct 18"; debugging overrides `mips2c_native_off` / `OPENGOAL_*_NATIVE=0` stay).
- `port_test/j2mirror` regenerated (small memory C, 840 modules; bones.gc and types-h.gc had
  changed), 3DS build rebuilt (`build-3ds-jak2`), all levels reconverted with the merged converter
  into `j2mirror/out/jak2/c3l`: 148 files, 570 MB, 171 s on the Mac (c3l_format.md).
- Checks: host small-memory C-mode `gk --ctr-gfx` with `jak2_city.pad` (Jak's green hair, the
  minimap, "Press (triangle) to use.", "Krimzon Guard:" in red with its capitals; Jak ends at
  (1065.98, 0.52, 194.46) in target-hide as before); Jak 1 host `level_pairs.pad` (exit 0, the same
  18 DGO loads and checkpoints as the previous pass); goalc-test 1708 passed; make_cia for both
  games (check_shaders 29 shaders, check_svc 31 calls: all allowed).

**Azahar (one run, the CIA, `jak2_city.pad`, private instance, sound off), slums with traffic:**

| | |
|---|---|
| fps | 29.0-29.9 (30 Hz cap) |
| game thread | logic 24.6-27.7 ms, waiting for the render thread 3.8-7.0 ms |
| render thread | build 25.6-29.3 ms: texture buckets 2.5 (the clouds change landed: 6.5 before), merc 3.8-5.0, shadows 2.9-3.9, sprites 3.6-4.3, sky 0.9, tfrag/tie (4 slots) ~9-10, debug/HUD ~1.3 |
| GPU busy | 26.3-30.0 ms, 64-79k triangles, 750-892 draws |
| memory | linear 32 MB: 131 KB free (ctyindb's load had to make room: "no memory for meshes, making room"); heap 68.4 of 72.2 MB used (~3.8 MB free); VRAM free 1654 KB |

The frame is now bounded by the 30 Hz cap with the GPU close behind (29-30 ms busy). Seen: the
"Krimzon Guard: Seal off the area." subtitle stays on screen from frame 1200 to the end in Azahar
(sound off), the host run shows it only briefly; not investigated.

**Staged:** `port_test/3ds-sdcard-jak2-baseline-b`: `cias/jak2.cia`, `3ds/jak2/` (gk.3dsx,
config.ini: preset normal, sound off, renderer ctr; `data/out/jak2/iso` 605 files,
`data/out/jak2/c3l` 148 files), README with what works and what's missing.

**Known gaps (baseline B):** no sound; texture animations other than the clouds and the sky
gradient; no ocean (black areas on the title screen), screen effects or envmap shine; the minimap's
circle mask; tie / shrub prototype visibility masks; linear memory nearly full in the city; other
districts not run in Azahar with this build; Jak 1's banner and jingle (Jak II icon only); no
hardware run. (Sound, texture animations, the ocean, the minimap and memory: see baseline C.)

## Integration build: baseline C (Oct 18)

(AI-assisted.) jak2-3ds after the j2-sound, j2-texanim and j2-mem merges (20e00088b7), plus the
title pads from j2-title (`jak2_title.pad`, `jak2_title_end.pad`) and `jak2_title_city.pad` (the
title pad's 3600 frames, then `jak2_city.pad` shifted by 2800 frames: one emulator run for both).
- `port_test/j2mirror` regenerated (small memory C; ocean.gc had changed), `build-3ds-jak2`
  rebuilt (.text 19.9 MB, .bss 2.4 MB), all levels reconverted with the merged converter (slot
  marks, texanim.c3t) into `j2mirror/out/jak2/c3l`: 149 files (147 levels + GAME + texanim.c3t:
  97 textures, 463 KB), 572 MB, 557 s on the Mac (ran next to the 3DS build; 171 s alone in B).
- Checks: host small-memory C-mode `gk --ctr-gfx`, `jak2_city.pad`: Jak's green hair, the
  minimap, "Press (triangle) to use."; Jak ends at (1065.99, 0.52, 194.46) in target-hide. (A first
  run with 6 pad shots, next to the Jak 1 run, ended at (1062.70, 8.00, 206.62) in target-stance:
  the soft renderer's host is paced at 60 Hz and the time ratio follows the real frame time, so a
  loaded host changes the walk; alone it matches.) Jak 1 host `level_pairs.pad` on proj-final:
  exit 0, 10 pairs, 18 DGO loads. make_cia (jak2): check_shaders 29 shaders, check_svc 31 calls,
  all allowed.

**Azahar (one run, the CIA, `jak2_title_city.pad`, private instance, sound on, instance muted;
staged files: compressed .ogz as on the card):**

| | |
|---|---|
| title (frames 600-3600) | 29.7 fps, logic 15.3 ms |
| slums standing (city-stand) | 29.6 fps; logic 21.5-22.5 ms, waiting for the render thread 8-9 ms |
| slums walking (city-walk) | 28.4-29.5 fps; logic 24.8-25.6 ms |
| render thread | build 26.9-27.9 ms (tex 2.6, merc 4.1-4.6, l3-tfrag 6.3-6.5, l0 2.0, l1 1.5, shadow 2.8-3.2, sprite 4.0, sky 1.0, debug/HUD 0.8) |
| GPU busy | 27.8-28.7 ms, 75-76k triangles, 835-854 draws |
| sound | mixer on core 1: 10-19% of real time, 15-22 voices; sound thread 3.7-6.6 ms/frame; 0 DSP frames dropped, 0 late buffers; CITY1 music, CTYWIDE1-3 banks, kg010 / kg015 guard lines |
| memory (slums, ctyindb dormant) | linear free 2.9 MB (B: 131 KB), heap free 3.3-3.5 MB (largest block 2.8-3.1 MB); ctyindb's load had to make room (128 KB linear free right after it) |

**Title (the key question): not fixed.** The intro scene still doesn't run to the JAK II logo.
What changed against baseline B: `*target*` is now at the PC's title position (1138.79, 33.40,
147.99) in `target-title` from frame 900 to 3601 (B: (980, 85, 346), the title-movie-end point),
the same position as the host run. What didn't: from frame 450 (a few seconds after
`title-disk-intro` starts) to 3600 every screenshot is the same still view, a city wall under the
sky (camera ~(958, 47, 359) m by the `[ctr] ctywide: shown again` line) with "Press the Start
Button" and black below it; no slums fly-through, no "Sony presents" / Naughty Dog cards, no
logo. The host (`gk --ctr-gfx`, `OPENGOAL_NO_SOUND=1`, sound engine running) plays the scene: the
slums street (450), the credits cards (600-750), Daxter (900), the logo from ~1050, "Press the
Start Button" with it. The intro's voice stream: on the host it mixes for ~40 s; on the 3DS the
mixer stats show 2 voices at +4 s and 0 voices by +15 s, so the stream (and with it the scene)
ends early even with audio output on. The scene's art was still loaded at the `continue` (frame
3800: "title-disk-intro+9 could not find a master slot to unlink"). Next: log the scene-player's
frame and the stream's play position (the IOP's `GetPlayPos` / the 989snd stream voice) over the
first 20 s on the 3DS; check whether the title's STR (the spooled animation) or the VAG stream
stops first. Screenshots and logs: `port_test/reports/j2-baseline-c`.

**Staged:** `port_test/3ds-sdcard-jak2-baseline-c`: `cias/jak2.cia`, `3ds/jak2/` (gk.3dsx,
config.ini: preset normal, sound on, renderer ctr; `data/out/jak2/iso` 605 files, compressed with
`--compress --compress-vag`: 4.52 -> 3.02 GB; `data/out/jak2/c3l` 149 files), README with what
works and what's missing. Sound needs `sdmc:/3ds/dspfirm.cdc` (not staged; Jak 1's).

**Known gaps:** the title intro (above; fixed after baseline C by j2-title2); screen effects; tie / shrub prototype visibility masks;
the ocean is Jak 1's flat approximation (opaque, no sky reflection or wave geometry); memory tight
at the market spot (ctyindb visible: 128 KB linear free, j2-mem table below); Jak 1's banner and
jingle; no hardware run.

Decompiler config: OpenGOAL's extractor database files v1 and v2.01 under one entry
(`ntsc_v1`). Both have the same serial and ELF hash, and the database lists both ISO hashes. The
`ntsc_v2` overrides only change label and anonymous-function types, so they only matter for
decompiling code. Data extraction and the build use `ntsc_v1`, as upstream does.

## Integration build: baseline D (Oct 22)

(AI-assisted.) Branch `j2-int-d` off baseline C (517021c5c8): j2-title2 (40457d03ad) merged, then
j2-mem2 (5d2006bb90); the only conflict was the status table here (both rows kept). No GOAL change,
so the C modules are baseline C's (`j2mirror/out/jak2/csrc`, linked from a private copy).
- Mixer stack: 64 KB (j2-mem2) with j2-title2's code: the mixer thread only adds
  `sceSdPublishVoiceAddrs` (a loop of relaxed atomic stores); the voice write locks are taken on
  the overlord's thread (`std::recursive_mutex`). Azahar: the 64 KB threads used 0.6-3.7 KB.
- Sizes: Jak II .text 19.19 MB, .data 19 KB, .bss 2.09 MB; Jak 1 3DS build .text 11.47 MB (links,
  CIA made). make_cia: check_shaders 29 shaders, check_svc 31 calls, all allowed (both games).
- Host checks (`build-host-small`, small-memory C mode):
  - `jak2_route.pad` with `CTR_STREAM_CHECK=1`: every stop and walk end matches j2-mem2's host
    run (industrial walk end (938.25, 7.99, 924.32), 0.6 m from it: run-to-run); 0 near versions
    without data; dormant check 4 / 2 (as before); exit 0.
  - `jak2_title.pad` (`OPENGOAL_VAG_DEBUG=1`, `OPENGOAL_NO_SOUND=1`): the intro stream runs to
    pos 38036 (j2-title2: 37788), 2 pause / unpause pairs; Jak at the title position; exit 0.
  - Jak 1 `level_pairs.pad` on proj-final: 10 pairs, 18 DGO loads, exit 0.
  - No audio path: the bank skip is 3DS only, so a temporary uncommitted host patch skipped the
    banks (`J2INTD_NO_BANKS`) with the streams still mixed: `jak2_title_city.pad`, 9 banks not
    loaded (COMMON, CITY1.MUS, CTYWIDE1-3, ...), no crash; the intro stream ran to 37844 and the
    title ended on the logo position; guard line kg010 played to its end (stop at pos 1020); Jak
    at (1065.98, 0.52, 194.46) target-hide as with banks; exit 0. Not run on a 3DS with sound off.
- **Azahar** (one run, the CIA, private instance, sound on, instance muted, compressed staging;
  `jak2_title_city.pad` then `jak2_route.pad` shifted by 4400 frames in the same session):

  | window | fps | logic ms | notes |
  |---|---|---|---|
  | title (600-3600) | 29.1-29.7 | 24.6-28.7 | intro to the JAK II logo (shots 450-1500: slums, Sony card, Naughty Dog card, city, logo from 1050, "Press the Start Button"); stream voices for ~35 s |
  | slums city-stand | 29.5-29.6 | 22.5-23.3 | render cpu 28.6-30.3 ms, GPU busy 27.8-29.4 ms, 82-85k tris |
  | slums city-walk | 28.9-29.5 | 23.8-26.9 | Jak ends at (1065.98, 0.52, 194.46) target-hide |
  | route stops (port .. sewer) | 29.6-29.7 | 17-25 (sewer 9) | |

  Mixer 4-21% of real time, 0 DSP frames dropped, 0 late buffers; sound lock waits <= 1 ms/frame.

  Memory (`[ctr] memory` lines in each stop's window: linear free (largest), heap free (largest),
  KB), against j2-mem2's table:

  | stop | linear free | heap free | j2-mem2: linear / heap |
  |---|---|---|---|
  | boot (frame 0) | 23326 | 14961 (14843) | 23326 / 14512: +449 KB heap, the stack cuts |
  | slums right after the title (ctyindb just loaded, in view) | **315** (156) | 7045 (6645) | not in j2-mem2's route (baseline C: 128) |
  | slums, ctyindb dormant (city pad, then route) | 4640, 3708, 3616 (~1550) | 7004-7036 (6645) | 5140, 4224 / 7828, 7529 |
  | port | 19263 (6388), 10302 (3496) | 10150, 7522 (6411-6678) | 9506 / 7965 |
  | market, standing | 7593 (3496) | 6882 (5618) | 8695 / 8376 |
  | market, ctyindb in view | **1270** (1108) | 6508 (5400) | 1804 / 6797 |
  | main town | 7790, 8259 (~3470) | 7540, 7645 (5924) | 8328, 8797 / 7885, 7787 |
  | industrial | ~7000 (3496) | 6998 (5286) | 7550 / 7379 |
  | farms | 12557 (3476), 3424 (908) | 8901, 7808 (5949) | 14527, 3512 / 9041, 7932 |
  | sewer | 4377 (3484), 3353 (2456) | 6391, 6328 (4532) | 4913, 3377 / 6661, 6537 |

  Boot memory is j2-mem2's plus the stack cuts. Later stops are 0.3-1.3 MB lower than j2-mem2's
  (and the largest blocks smaller): this session first played the whole intro and the city pad
  (5200 frames) before the route (j2-mem2's route cut the intro at frame 1000), and the stream
  arena grew to 3584 KB here (j2-mem2: at most 3008). No `[heap]` failures, no "making room", no
  incomplete loads.
- Loose ends from j2-mem2:
  - The second boot after the pad's `exit`: not the game. `exit` ends the runtime cleanly (status
    2 = `RuntimeExitStatus::EXIT`; `main` only loops on `RESTART_RUNTIME`, then returns), and the
    second session in j2-mem2's log starts with the startup banner of a new process. In this run
    the same exit was followed by Azahar's `~Process: Cleaning up process` and nothing for the
    remaining ~230 s until the run script stopped the emulator. So it was a relaunch from outside
    the game (emulator / launcher), harmless.
  - The black shot 4800: game frame 4800 is ~23:12.9 in j2-mem2's log (frame 4418 at 23:00, 4866
    at 23:15), between the farms `continue` (23:12) and Jak standing (23:15): the continue's
    blackout while ctyfarma loads. This run has the same thing once: shot 6150, in the port
    continue (57:45-57:47).
- Staged `port_test/3ds-sdcard-jak2-baseline-d` as baseline C (`stage_sd.sh --game jak2
  --compress --compress-vag`, preset normal, sound on): `cias/jak2.cia`, `3ds/jak2/` (gk.3dsx,
  config.ini, data: 754 files, byte-identical to baseline C's), README. gk.3dsx and the CIA are
  byte-identical to the build. Logs and screenshots: `port_test/reports/j2-baseline-d`.

## Title intro on the 3DS (j2-title2, Oct 18)

(AI-assisted.) **Symptom** (baseline C, Azahar, sound on): the intro scene (`title-disk-intro`)
froze ~1 s in on a city wall; its voice stream went quiet; the host played it all (~37 s).

**How it was found:** the VAG stream log (`[vagdbg]`, spustreams.cpp: `OPENGOAL_VAG_DEBUG=1` on
the host, `-DOG_VAG_DEBUG` in a 3DS build): the stream state every 250 ms, every pause / restart /
stop, the STR part loads. The scene player (`ja-play-spooled-anim`) sets its frame from the
stream's play position (`current-str-pos`), which the overlord computes from the voice's address
in its double buffer (`GetVAGStreamPos`: buffer half flips + the address in the half). Host: the
position rises ~1024 per second to 37788. Azahar: the first pause / continue pair of the stream
(the scene does one when its next STR part isn't loaded yet; a normal thing) was followed by a
voice address of 0x8000164a in a 0x4000 buffer and 94 buffer flips in 0.1 s: the position jumped
~30 s ahead, the scene ran through parts 2-9 in a second ("loader pre stall" lines), then the
stream (and the scene's frame) stood still for good.

**Cause:** the 3DS mixer (core 1) runs each voice a 128-frame chunk at a time with its decoder
state in registers (`Voice::RunBlock`) and writes it back at the end. The overlord (IOP, core 0)
keyed voices on / off (`sceSdSetSwitch`) and set their addresses without any lock, so a key on in
the middle of a chunk was undone or mixed with the old state: a garbage NAX. The host mixes
sample by sample (`Voice::Run`), where the same race is practically harmless.

**Fix** (game/sound, 3DS only in effect):
- `sceSdSetSwitch` (key on / off) and `sceSdSetAddr` (start / loop address) take the mixer's
  lock (`snd_LockVoiceWrites`), so they land between chunks. Volume / pitch / ADSR writes (single
  fields) stay unlocked. Lock waits: title ~0.1 ms/frame, city as in baseline C.
- `sceSdGetAddr` returns the address the mixer published after its last chunk for all 8 overlord
  voices at once (`sceSdPublishVoiceAddrs`, `Voice::GetPublishedNax`; a key on publishes at once).
  Read in the middle of a chunk, a stereo stream's two voices were up to a chunk apart, which
  `GetVAGStreamPos` took for a drifted pair and re-synced with a pause / continue: ~15 clicks a
  second through the whole intro (570 pairs); now 2 in the intro (the host has 1).
- Jak 1: its overlord only writes voice registers (no `sceSdGetAddr`), so only the lock applies
  there; the host is unchanged.

**Checked:** host (C mode, `.ogz` files): stream to 37788, logo; Azahar (the CIA, compressed
staging as baseline C, sound on, instance muted, `jak2_title_city.pad`): stream to 37744, the
slums fly-through (450), "Sony Computer Entertainment America presents" (600), the Naughty Dog
card (750), the city (900), the JAK II logo from 1050 with "Press the Start Button"; title
27-29.7 fps with logic 25-28 ms (the scene now runs: baseline C's frozen title had 15 ms), then
the city as before (Jak at (1065.98, 0.52, 194.46) in target-hide, guard lines kg010 / kg015, 0
DSP drops). Screens: `port_test/reports/j2-title2`.

## Title intro: missing geometry at the start (j2intro, Oct 24)

(AI-assisted.) **Report** (user, Azahar, Japanese): "in the game intro, haven city is initially
displayed with lots of missing geometry".

**What was missing:** the slums themselves (ctysluma: houses, street, the tower) for the first
seconds of `title-disk-intro`. Only ctywide was drawn (the city wall, the billboards, the sky), with
black below it and citizens floating in it. Baseline D's Azahar log: the intro's voice stream (and
with it the scene) started at 54:28, and ctysluma's .c3l came in at 54:30, so the scene was ~2 s in.
In this session's Azahar run of the user's CIA (oct23b), ctysluma happened to arrive 5 s before the
intro, so the frames were complete: it's a race. The host loads a .c3l in the frame it's wanted, so
it never shows this. A host model of the SD card's load time (`CTR_LOAD_DELAY_MS`, below) reproduces
it every time: `reports/j2intro/host_sdmodel_before_*.png`.

**Why:**
1. Jak II never waited for a level's background. Jak 1's `level-update-after-load` waits for
   `pc-3ds-level-ready?`, but Jak II didn't call it. Jak II also reports a level to the renderer
   (`__pc-set-levels`) only once it is `loaded`, so the .c3l read only began after the DGO was
   in and logged in. The title waits for ctysluma and ctywide to be `active`, then starts the scene
   at once.
2. The .c3l reads were slow at the title: ctywide 9-10 s, lwidea 2.3 s, ctysluma 7.8-11.3 s, one
   after the other. The level loader runs on core 2 below the render thread. The render thread
   spent 76 ms a frame in the tex bucket on the Dolby card, so the loader starved:
   `draw-raw-image` sends the 512 x 416 card every frame, and `CtrVram::upload_ct32` rewrote it.
   That gave its chunks new ids, so the card was decoded again every frame.
3. At the reveal: ctywide went dormant before the intro (its string was all zero for 90 frames), so
   its 36 freed tie chunks were read back after the cut. The near versions of the tie LOD groups in
   view were also late after the cut (Azahar ~400 ms). Without a black screen, j2-mem's and
   streaming's holds kept them hidden or far in view for up to 4 s.

**Fix:**
- `goal_src/jak2/engine/level/level.gc` (TARGET_3DS): `level-update-after-load` stays in login
  until `pc-3ds-level-ready?` (game/kernel/jak2/kmachine.cpp -> `ctr_level_ready`, at most 20 s
  per level). The scene starts when the levels are in, and the stream starts with the scene, so
  the audio stays in sync.
- The .c3l is read from the start of the level's DGO. The Jak II overlord's `LoadDGO` calls
  `ctr_level_dgo_started`, which maps the name through `ctr_jak2_level_for_dgo` and prefetches.
  Without this, a level waiting in login would never be asked for. Jak II's prefetches aren't
  limited to Jak 1's two level slots (`can_start`: 6 for Jak II). With two, lwidea waited 20 s
  behind title + ctywide.
- `CtrVram::upload_ct32`: an image that is already in VRAM (`ct32_same`) isn't written again, so
  the textures decoded from it stay valid.
- Black hold (Jak II; Jak 1's title3 does the same on batch-oct11): `CtrRenderer::jak2_hold_black`.
  A screen coming out of the game's black (pmode alpha = 1 - bg-a: blackouts and fades) within 3 s
  of a level shown again (dormancy ended) or a camera cut stays black while something in view is
  missing: tie LOD groups drawn far for late near chunks, or dormant chunks not back
  (`CtrLevels::note_late_in_view`). At most 1.5 s, then a fade in over 8 frames. While the screen
  is black (`ctr_level_set_screen_black`), nothing is kept hidden or far in view for late data.
  The game runs on behind the black, so the scene clock (the voice stream) isn't touched.
- Host model `CTR_LOAD_DELAY_MS="level:ms,..."` or `"*:ms"`: a level load is handed over that long
  after it ran; with it the host's `pc-3ds-level-ready?` follows `ready()`. `CTR_LOAD_NO_WAIT=1`
  gives the model without the game's wait, i.e. the 3DS before this fix. `CTR_UPLOAD_LOG` also
  logs raw images written or skipped.

**Checked:**
- Host (`gk --ctr-gfx`, small-memory C mode, every frame dumped):
  - Without the model, intro frames 326-389 are the same before and after.
  - With `CTR_LOAD_DELAY_MS="ctysluma:20000,ctywide:6000"`:
    - before (`CTR_LOAD_NO_WAIT`): only ctywide;
    - after: the game waited 19.8 s for ctysluma, then the intro's first frame is complete (black
      held 3 frames).
  - The Dolby card: 193 of 194 uploads skipped.
- Azahar: one run of the CIA with the user's config (sound on, instance muted, compressed staging
  as oct23b), `jak2_title_city.pad` with a screenshot every frame 250-1100:
  - Loads: ctywide 1.9 s, lwidea 0.8 s, ctysluma 6.2 s (the game waited 5.3 s for it).
  - The intro started 22 s after the title level was in (it was 34-38 s).
  - Black held 469 ms (5 frames at the screenshot-slowed rate). The first visible frame (330) is
    complete.
  - The tex bucket on the Dolby card took 22-24 ms a frame (it was 76).
  - Title 29.6 fps outside the screenshot window. The city as before: Jak at (1065.98, 0.52,
    194.46) in target-hide; linear 307 KB free right after ctyindb's load (baseline D: 315).
- Jak 1: host `level_pairs.pad` (proj-final), exit 0, 18 DGO loads.
- Strips and logs: `port_test/reports/j2intro`.

Left: the intro's later camera cuts (no black between them) still show tie LOD groups far for
~0.4 s (streaming's normal late-data path).

## Milestone 2: logic-only boot on the 3DS (Oct 18)

(AI-assisted.) Jak II boots in Azahar (New 3DS, the CIA) with the null renderer and reaches Haven
City with traffic (`jak2_city.pad`). Build and packaging: 3ds_build.md "Jak II build"; module
instances: c_backend.md "Shared object files".

**City frame time (Azahar, CIA, 804 MHz mode, null renderer, 30 Hz cap):**

| Window (jak2_city.pad) | fps | logic ms/frame | notes |
|---|---|---|---|
| title screen | 30.0 | ~26 | |
| city-stand (600 frames, slums, 291 processes) | 13.2 | 75.6 | iop 2.3 ms, 143 thread suspends/frame, 25 KB stack saved/frame |
| city-walk (300 frames) | 13.4 | 74.7 | |

The `perf:` lines' logic is kernel dispatch minus the vsync wait; with the null renderer there is
no render wait, so it is all game logic (plus the IOP thread's 2.3 ms on the same core). The Mac
estimate was 50-70 ms. Jak 1 on Azahar ran about at hardware speed for logic, so expect ~75 ms
on a real New 3DS as well (not measured). Jak's end position (1065.98, 0.52, 194.46) target-hide
is the same as on the PC. Logs: `port_test/3ds-sdcard-jak2-baseline-a` (staging) and this
session's run; jak2-prof works on the logic profile.

**Small memory layout** (`OPENGOAL_SMALL_MEMORY=1` for goalc, on for the 3DS; same 48 MB layout as
Jak 1):
- `gcommon.gc`: SMALL_MEMORY / TARGET_3DS from the environment, BIG_MEMORY / PC_BIG_MEMORY off,
  END_OF_MEMORY #x3000000, `*goal-small-memory*` (checked by the Jak 2 kernel, kscheme.cpp).
- PROCESS_HEAP_MULT 1 (PS2: *nk-dead-pool* 1540 KB, the city uses 918-1078 KB), SPRITE_MAX_AMOUNT_MULT
  off (PS2 sizes), DEBUG_LEVEL_HEAP_MULT 1.0 (level heap 18396 KB, the PS2's 146 pages),
  DMA_BUFFER_SMALL_MULT 1 (2 x 1280 KB; `*dma-mem-peak*` in the memory report: 199-284 KB on the
  route).
- Jak 2 kernel: stack at GOAL_KERNEL_STACK_TOP (0x2c00000), `*stack-top*` etc. to match, debug heap
  the 4 MB above it.
- Result (host small-memory C mode and Azahar): global heap 30.5 of 42.5 MB used in the slums
  (level-heaps 18396, scheme-globals 5436, dma-buffers 2585, process-buffers 1754, tpages 879 KB). Level
  heaps at 1.0x: ctysluma 5843 / ctyindb 5949 of 6048 KB, ctywide 3402 of 3825, lwidea (borrow)
  2088 of 2095 KB: the tightest. `jak2_route.pad` (slums, port, market, main town, industrial,
  farms, sewer) passes on the host with these sizes.

**Shared object files:** fixed with module instances (c_backend.md). The Jak 2 3DS build compiles
108 objects twice (+1.7 MB .text). Host A/B test `jak2_link_copy.pad`: with instances Jak walks to
the same place as in `jak2_city.pad`; with one instance (old behaviour) he never moves.

**Boot fixes found on the way:** `iso.h` open_wad / sync_read function types (int32_t is `long` on
ARM newlib); sqlite and the REPL user name stubbed on the 3DS; `-fakeiso` in the default args (the
Jak 2 kernel otherwise reboots the IOP from the DVD and the RPC binds fail: `sif_busy` assert);
the CIA is needed (a .3dsx gets 62 MB of heap); per-frame "Kernel dispatch time" prints off on
the 3DS (one SD write per slow frame).

**Staged:** `port_test/3ds-sdcard-jak2-baseline-a` (LOGIC ONLY, black screen, no sound; README in
it): `cias/jak2.cia`, `3ds/jak2/` with gk.3dsx, config.ini, data/out/jak2/iso (605 files).

**Next:**
1. Logic speed (13 fps in the city): jak2-prof's profile; the Jak 1 optimisations (native mips2c,
   32-bit codegen) apply. 143 thread suspends/frame with 25 KB of stack copied is a target too.
2. Renderer: the Ctr renderers adapted to Jak 2's buckets (background via the converter on Jak 2
   fr3, merc, sprites/HUD/text, ...); linear heap back up from 8 MB.
3. Sound (Jak 2 overlord streaming on ndsp), then a hardware run.
4. Banner (Jak II logo model) and jingle for the CIA; it has the icon only.

## Renderer: level backgrounds (j2-render-bg, Oct 18)

(AI-assisted.) Branch `j2-render-bg`. Jak II's tfrag, tie and shrub (every kind: normal, alpha,
water, envmapped ties' base pass) come from `.c3l` files made from Jak II's fr3 files by the same
converter (`c3l_format.md` "Jak II"), loaded by the same `CtrLevels`, drawn by
`CtrTfragRenderer::render_jak2` (`CtrRendererJak2Bg.cpp`: one renderer per level slot 0..5, set on
all of the slot's background buckets; the first bucket with the level's camera, the
`add-pc-tfrag3-data` packet, draws the whole level). Time of day (the level's mood itimes, as
Jak 1), fog (`*fog-color*` from bucket 2), the game's visibility strings (bucket 2, all 6 slots;
an all-zero string hides the level, as the PS2 and PC do: ctyindb seen from the slums), tie levels
of detail with streaming, impostors, run culling, vista tier: all as in Jak 1. Jak II's GS y
scale (`ctr_gs_y_scale`, j2-render-fg) in the level matrix. Jak II has no hfrag (Jak 3 only).
The foreground (merc, sprites, HUD, text, sky, texture uploads) and the Jak II dispatch are
j2-render-fg's.

**Host (`gk --ctr-gfx`, `jak2_city.pad` plus shots):** the slums match the PC OpenGL reference
(same geometry, lighting and fog; side-by-side shots in the j2-render-bg report). ~72-78k
triangles per frame in the slums (ctysluma + ctywide; ctyindb loaded, hidden by its string).

**Azahar (CIA, 804 MHz, `jak2_city.pad`, Oct 18):** the city draws.
- Render thread (core 2): build 20-22 ms per frame, GPU busy 21-23 ms at 76-80k triangles, ~1150
  draws (the GPU model 10.6 ms + 0.13 ms per 1000 triangles: ~21 ms). `auto_detail` climbs to
  1.00 (shrubs 120 m, ties x3) because the frame is logic-bound.
- Game thread: logic 74-77 ms (13 fps), the same as with the null renderer: rendering doesn't slow
  the logic.
- Memory (the problem): linear 32 MB, 128 KB free in the slums with ctyindb loaded (its load
  had to make room); level pools 2.8 MB, stream arena 4 MB full. Heap ~72.3 MB, 67.6 MB used:
  the renderer adds ~12 MB of heap (~6 MB fixed, ~2 MB per district).

Not done yet: tie / shrub prototype visibility masks (Jak II hides prototypes by game state;
everything is drawn), animated textures in the background (untextured, e.g. the billboards are
black), the ocean, `ctr_level_ready` (the game doesn't wait for a level's `.c3l`), the DGO
prefetch (`ctr_jak2_level_for_dgo` exists, nothing calls it yet), Jak II interiors for the vista
tier.

## Renderer: foreground (j2-render-fg, Oct 18)

(AI-assisted.) Branch `j2-render-fg`. Jak II's bucket table and dispatch for the Ctr renderer
(`CtrRendererJak2.cpp`, `CtrRenderer.cpp` `dispatch_buckets_jak2`: the chain starts with bucket 0,
`vif_interrupt_callback(bucket + 1)`), the foreground renderers on Jak II's DMA. Everything Jak II
is behind `ctr_is_jak2()` / the bucket table; Jak 1 keeps its constants.

| What | How | Notes |
|---|---|---|
| Screen y | `ctr_gs_y_scale()`: 512 / 416 * 0.5 (Shader.cpp SCISSOR_ADJUST * HEIGHT_SCALE); direct draws 1 / 208 | Jak 1: 512 / 448, 1 / 112 |
| Merc (all 6 level slots + lcom, every texture group) | `CtrMercRenderer`: no uses-water quadword, one link per model, bit 3 = disable-envmap | Jak's face / hair / fingers / eyebrows: the converter bakes the animated slots' index textures with their `-norm` CLUT (`add_anim_slot_textures`); Dark Jak's blend not done |
| Eyes | `CtrEyeRenderer` from the texture buckets' eye DMA (no eyes bucket in Jak II) | not checked up close |
| Texture uploads | `CtrJak2TexRenderer` on every TEX bucket; `CtrVram` reads textures the PC port moved (`__pc-get-tex-remap`, +1..4 blocks) from their data | the city signs and the glow were block-scrambled without it |
| Texture animation | `CtrTexAnimJak2`: clouds (CPU port of `run_clouds`, 64 x 64, every 4th frame), generic uploads (CT32 images: the Dolby logo; PSMT8 + its CLUT: the sky's fog gradient), image data copied on the game thread | not done: Dark Jak / prison / oracle CLUT blends, waterfalls, lava, security walls, slime and the other fixed animations (their textures keep the converter's) |
| Sky | the game's sky-work GIF packets (`SKY_DRAW`, direct), the clouds above | ~70k vertices a frame (stars, cloud grid) |
| Sprites / particles | `CtrSpriteRenderer` on `PARTICLES`: Jak II frame data, matrix -1 sprites skipped; glows: the glow VU program's flare math, added, depth tested at the flare center (no probe read back) | distorter off for Jak II |
| Shadows | `CtrShadowJak2Renderer` (SHADOW, SHADOW2): Shadow2's volumes into Jak 1's stencil passes | darkened by multiplying (the PC subtracts) |
| HUD / text | direct buckets (progress, debug2, ...); font relocations use the game's font heights (setup-font-texture's 800) | the letters A-K and the button symbols were missing before |
| Minimap | the progress bucket's FBP 126 draws go to a 128 x 128 render to texture (XYOFFSET, tbp 4032), drawn as the circle in its rectangle | the PS2's alpha-only circle mask isn't kept; left out |
| Boot logos, title | Dolby logo, "Press the Start Button" | |

**Azahar (one run, the CIA, `jak2_city.pad`, before the clouds change):** title, Dolby logo, the
slums with merc, HUD, text, minimap, particles, shadows. Render thread 27-29 ms/frame build (of it
foreground: texture buckets 6.5, of which the clouds most (now ~1.5 by the host ratio), merc
3.4-4.2, shadows 2.6-3.1, sprites 2.2-3.0, sky 0.9, HUD/text ~1.1; background ~8.5), GPU busy
28-30 ms/frame, 44-57k triangles, 574-739 draws; game thread logic 45-47 ms (21 fps: logic bound).
Linear free 128 KB in the slums (the level pools), VRAM free 1654 KB.

**Memory the foreground adds:** heap ~180 KB (clouds 95 KB, CtrVram's Jak II tables 80 KB; the
logo snapshot's ~850 KB at boot is given back); GPU textures 24 KB linear (clouds, fog gradient)
+ the minimap's 128 x 128 render target and its D16 tile (VRAM while there is room).

**Host checks:** `gk --ctr-gfx` small-memory C mode (`j2fg` mirror) against the PC OpenGL
reference (native, same pad): title, slums standing / turning / walking. Jak 1: level_pairs.pad,
the 10 pairs' shots against a gk without these changes: only run-to-run differences (particles,
Jak's idle, the time of day); Jak 1 3DS build links, make_cia.

Host diagnosis switches (no effect on the 3DS): `CTR_SKIP_BUCKETS=",id,"`, `CTR_DIRECT_STATS`,
`CTR_DIRECT_DUMP=",bucket,frame,"` (+ `CTR_DIRECT_DUMP_N`), `CTR_UPLOAD_LOG`,
`CTR_TEX_DUMP=<dir>` (+ `CTR_TEX_DUMP_RGB`), `CTR_GLOW_LOG`, `CTR_MINIMAP_DUMP=<png>`.

## Texture animation, ocean, minimap (j2-texanim, Oct 18)

(AI-assisted.) Branch `j2-texanim`.

| What | How | Where it shows |
|---|---|---|
| CLUT blends: Dark Jak (`jakbsmall-*`), prison (`jak-orig-*`, `jakb-*` LDJAKBRN), oracle, nest | `CtrTexAnimJak2::clut_group`: ClutBlender's blend of the two CLUTs, then the index lookups, at most 64 x 64; only when the game's weight changes (weight 0: the level's base texture, baked with the first CLUT, is the same) | Jak's hair / face / fingers / eyebrows going dark |
| Fixed animations: security walls, waterfall(s), lava, conveyors, skull gem, bomb gradient, stadium wall, robotank treads, fortress screen, metal kor / shield / krew holograms | `CtrTexAnimJak2Defs.h` (TextureAnimatorDefs.cpp's Jak II definitions as tables), `draw_layer`: each layer's quad, colour and texture coordinates as the game sends them, bilinear, the two blends; fixed point, row spans | ctywide (security walls), forest + mountain (waterfall), dig1 + dig3a (lava), castle, fortress levels...; skull gem and bomb gradient are sent in every level |
| Slime | the clouds' noise (shared random table) + the PC's slime LUT, 64 x 64, scrolled copy | castle |
| Where the results go | the converter marks each slot's base texture in a level (`c3l::texture_anim_slot`); `ctr_gpu_tex_set_anim_slot` at load; draws of such a texture use the slot's texture (`ctr_gpu_anim_slot_set`, taken at the next frame begin, so a frame never mixes two) | every level and merc draw of a slot |
| Ocean | `ocean-3ds-packet` (ocean.gc, TARGET_3DS): Jak 1's OCN3 packet (the map, this frame's wave heights, camera) plus the sky's light toward the view (add-colors!); `CtrOceanRenderer` (Jak 1's flat ocean) in ocean-near with Jak II's y scale, the wave texture times the square root of that light. The EE no longer builds the PS2 ocean's vertices, wave texture, far / mid / near DMA | title backdrop, the port, the city's edges |
| Minimap | the circle (already drawn as a fan instead of the PS2's alpha-only mask) now opaque: the mask gave the whole circle alpha 1, the map's own alpha let the scene through (black roads were gray, the sea cyan) | HUD |
| Envmap / emerc shine | nothing new: CtrMerc's envmap path already draws Jak II's envmap draws (`envmap batches` in the merc stats; 35 envmap draws in garage alone) | guards, vehicles |

Not done: VRAM copies of the move-to-pool animations (oracle / nest blends, skull gem, fortress
screen, metal kor / shield / holograms: only the generic renderer, which the 3DS doesn't have, read
them there); the PS2 ocean's sky reflection and transparency near the shore (the flat ocean is
opaque, one colour from the map, shaded by the waves).

Ocean next to the PC: daytime the port's water is the map's teal with the wave shading (the PC's
is lighter and reflects the sky); at night darker and less blue than the PC's (port, average colour: PC (6, 22, 43); 3DS
(17, 46, 55) untinted, (3, 9, 14) with the plain tint; the square root of it now in use sits between,
not measured again); no waves' geometry (flat at the map's
height; `ocean = waves` would add Jak 1's near grid, not enabled for Jak II), no transparency.

Memory: texanim.c3t sources loaded per animation (slums: 68-84 KB of heap), GPU textures 4-8 x
up to 16 KB linear (x2 while updated: the pipeline's second copy), freed 10 s after an animation
stops; the ocean's mesh (~140 KB linear, 8.8k vertices, 15k triangles at the port) and its 32 x 32
texture.

Host checks (`gk --ctr-gfx`, C mode, `platform/3ds/tests/jak2_texanim.pad` and the PC renderer on
the same pad; images in port_test/reports/j2-texanim): title, slums, forest, dig1, port day and
night; Dark Jak with `CTR_TEST_CLUT_WEIGHT=1` (host diagnosis); Jak 1 `level_pairs.pad` exit 0, all
10 pairs.

**Azahar (the CIA, `jak2_texanim.pad`, private instance, sound off), two runs:**
- First run (every fixed animation every 2nd frame at 64 x 64): 3.2 ms of render thread per
  update; the slums sent bomb gradient, skull gem and both security textures every frame: texture
  buckets 8.7 ms (2.5 before), build 30.6 ms. Hence the second version: an animation runs only
  while its slot was drawn last frame (`ctr_gpu_anim_slots_drawn`), at 32 x 32, at most every 3rd
  frame and one animation a frame.
- Second run: slums with traffic 28.4-29.7 fps, render thread build 26.85 ms (texture buckets 2.59,
  as before; no security wall in view: 0 updates), GPU busy 27.6 ms, 65k triangles; logic 26.5 ms.
  Forest: the waterfall ~1.3 ms per update, every 3rd frame (~0.45 ms a frame); dig1: the lava
  ~1.2 ms per update. Port: the ocean 0.25-0.36 ms of render thread (the mesh is built once per map).
- Memory in the slums: linear free 144 KB (131 before), heap ~68.6 MB used of 72.1 (as before):
  the slots' textures and sources are only allocated while an animation runs.

## Paths

| What | Where | Needed to resume? |
|---|---|---|
| Worktree (source, branch jak2-3ds) | `jak-project/.claude/worktrees/jak2` | yes (commits are in the repo) |
| Native PC build | `<worktree>/build` (Release, arm64, Ninja, `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`) | regenerable, ~15 min |
| Extracted disc | `jak-project/iso_data/jak2` (real files, main checkout, git-ignored); `<worktree>/iso_data/jak2/*` are symlinks to it | keep (regenerable from the ISO in ~5 min) |
| Decompiler data output | `jak-project/decompiler_out/jak2` (`<worktree>/decompiler_out` symlinks to the main checkout's) | regenerable, ~1 min |
| Native game output | `<worktree>/out/jak2` (fr3, obj, iso) | regenerable: decompile and `(mi)` |
| C-mode mirror | `port_test/j2mirror`: symlinks to the worktree except `out/`; `out/jak2/{csrc,cmod,obj,iso}`, `out/jak2/fr3` symlinked to the worktree's | regenerable, ~2.5 min |
| Project-path link | `port_test/j2link/jak-project` -> worktree (binaries find the project by the last "jak-project" in their path) | yes (tiny) |
| Logs | `port_test/j2data` (extract, decompile, native and C build logs; `cbuild1.log` = the lenient C build, `cbuild6-strict.log` = the strict one) | no |
| Host small-memory gk (Jak 1 check) | `<worktree>/build-host-small` (Release, `-DOPENGOAL_SMALL_MEMORY=1` C and C++ flags) | regenerable, ~10 min |

The VAG/STR/MUS copies in both `out/jak2/iso` folders are APFS clones of `iso_data/jak2` (`cp -c`),
so they take no extra space. A new `(mi)` copies them again, so clone them again afterwards (see the
commands below).

Disk (du counts clones in full): iso_data 4.1 GB, decompiler_out 0.5 GB, worktree source ~0.6 GB,
build 1.3 GB, worktree out 5.4 GB, j2mirror out 5.0 GB. Space actually used is about **9.6 GB**:
the out/ iso folders hold only ~0.6 GB each of their own data (DGO/CGO/TXT); the rest is clones.

## Commands

```sh
W=/Users/user/Projects/port_test/j2link/jak-project      # the worktree, through the link
# extract (into a staging dir, then move into the main checkout's iso_data/jak2 and symlink)
$W/build/decompiler/extractor "<Jak II iso>" --proj-path $W --extract-path port_test/j2data -g jak2 -e
# decompile data (fr3 levels, textures, text)
$W/build/decompiler/extractor $W/iso_data/jak2 --folder --proj-path $W -g jak2 -d
# native compile ((mi) with *iso-data* set)
$W/build/decompiler/extractor $W/iso_data/jak2 --folder --proj-path $W -g jak2 -c
# C-backend compile into the mirror (drop LENIENT for a strict build)
OPENGOAL_C_BACKEND_LENIENT=1 $W/build/goalc/goalc --game jak2 --proj-path port_test/j2mirror \
  --instruction-set c -c '(mi)'
# re-clone the copied audio after a (mi)
cd <out>/jak2/iso && for f in *; do s=$(find -L $W/iso_data/jak2 -name "$f" -type f | head -1); \
  [ -n "$s" ] && cmp -s "$f" "$s" && rm "$f" && cp -c "$s" "$f"; done
# silent PC run (OpenGL window; screenshots via the pad script's "shot", one file:
# <config>/OpenGOAL/jak2/screenshots/screenshot.png)
OPENGOAL_NO_SOUND=1 OPENGOAL_PAD_SCRIPT=city.pad $W/build/game/gk -g jak2 --proj-path $W \
  --config-path <scratch>/cfg -- -boot -fakeiso -nosound
# C mode: add -cbackend, run from the mirror (a scratch --config-path keeps the user's settings)
OPENGOAL_NO_SOUND=1 OPENGOAL_NULL_GFX_FPS=30 $W/build/game/gk -g jak2 --proj-path port_test/j2mirror \
  --config-path <scratch>/cfg --null-gfx -- -boot -fakeiso -nosound -cbackend
# windowed (OpenGL) runs while the screen is locked or the display sleeps: add
# OPENGOAL_GL_NO_VSYNC=1, otherwise the macOS buffer swap waits forever (native too)
# ABI check mode: move out/jak2/cmod aside, rm out/jak2/obj, build with
#   OPENGOAL_C_BACKEND_CC="cc -DGOALC_ABI_CHECK", run with OPENGOAL_C_ABI_CHECK=1;
# stack poison: OPENGOAL_C_BACKEND_CC="cc -DGOALC_STACK_POISON" the same way. Put the normal cmod back
# (or rebuild it) afterwards: the module hash doesn't include the cc flags.
# kernel function ABI audit (compile time, like Jak 1's CAbiKernelFunctionTypes test):
OPENGOAL_C_ABI_KERNEL_DUMP=<scratch>/kdump.txt gk ... -cbackend      # a few seconds of boot
$W/build/goalc/goalc --game jak2 --proj-path port_test/j2mirror --instruction-set c \
  -c '(begin (mi) (__c-abi-check-kernel-functions "<scratch>/kdump.txt"))'
# frame timing: "perf: ... logic ..." lines once a second (kperf, as Jak 1); average them per
# "perf-mark" window of the pad (platform/3ds/tools/perf_report.py's regex doesn't match the host
# lines' spacing yet)
```

Notes:
- `-boot -debug` (boots to the prison, without the title) segfaults natively on arm64 within
  ~10 s. Not investigated.
- `-level ctysluma` fails: `bg` looks for `CTYSLUMA.DGO`, but the file is `CTA.DGO`.

## Pad script on Jak 2

The hooks for `print`, `wait`, `pos`, `continue` and `exit` are in `game/kernel/jak2/kscheme.cpp`
(Jak 2 layouts: process state +64, process-drawable root +128). `continue NAME` calls the GOAL
`pad-script-continue` (`goal_src/jak2/pc/features/speedruns.gc`). That function spawns a process in
`*pc-pool*`, because the hook runs inside `cpad-get-data` and `reset-actors` kills
`*entity-pool*`. The process then does the speedrun act-2 setup: mode `debug`, `initialize!`, mode
`play`, closes `city-oracle-level0-training` and `ruins-enemy-resolution`, `start 'play`.

**Open problem.** From the title screen, Jak ends up in `target-title` again. The title level stays
loaded and its title-control keeps re-spawning. The helper kills it every frame for 1200 frames, but
that didn't help: the last run still showed `target-title` 900 frames after `start`.

Next things to try:
- (a) wait until the title menu is up, then press X / navigate (`progress-intro-start` path);
- (b) take the title level out of `*load-state*` before `start`, e.g. `want-levels` with the
  continue's levels, or `(set! (-> *load-state* want 0 name) ...)`;
- (c) find who sends `change-mode`/`go target-title`: title-obs.gc `target-title`, or a continue
  point with the `title` flag (`title-start`/`title-restart` from `initialize!`);
- (d) boot without the title: `*kernel-boot-message*`, or make `-level` map to the DGO name.

## C-backend warnings (from `j2data/cbuild1.log`), reviewed Oct 18

(AI-assisted) The bug class that broke Jak 1's platforms (c-abi-v3-narrowing). Branch jak2-abi
fixed the real ones; a C build after the fixes has 310 warnings, all shown harmless below.

Narrowing casts (11 -> 6):
- fixed: `warp-gate.gc` `target-warp-out` arg2 (`target` cast to `handle`): the warp gate sends its
  handle through `change-state`, the `target`-typed parameter dropped the pid, so `handle->process`
  failed and the gate never got `'effect`. arg2 is now `object` (target-h.gc): it is the gate's
  handle, or the entity that `target-warp-in` passes on (a pointer, the same as native).
- fixed (was harmless): civilian / guard / crimson-guard-level / crimson-guard-hover passed
  `(the-as handle this)` to `speech-control-method-13`, which compares it with `handle->process`:
  it was a process all along, never a real handle, and passed whole (hi = 0, as native).
  `speech-control-method-13` and `speech-channel-method-10` now take a `process`.
- harmless: `timer.gc` puts the frame delta (a small `time-frame`) into `sparticle-data x` as float
  bits; the low half is the whole value, and a float field store keeps only it on every backend.
- harmless: 5x `gs-tex0/tex1/clamp` cast to `adgif-shader` in `default-menu.gc` (debug): the value
  is cast straight back to `int` and shifted, never passed as an argument (locals keep 64 bits).

Function-type casts (306 -> 304):
- fixed: `editable-player.gc` (debug editor): a `(function editable symbol symbol)` lambda called
  with an `editable-region`; `symbol` is WIDE in Jak 2, so `(= (-> arg0 region) arg1)` compared a
  garbage upper half. The lambda takes `editable-region`, like its twin a few lines below.
- fixed: `menu.gc` int items (debug menu) called their `(function int debug-menu-msg int int int)`
  callbacks through the float view: the ints got a garbage upper half. They now store and call
  `iactivate-func` with ints, as Jak 1 does.
- harmless, the affected parameter is never read (a WIDE parameter only reads a float register):
  300 bot course callbacks (`*-course.gc`: sewer mog/jinx/grim/hal, atoll sig0, under sig5, city
  kor/kid/hal3, ash4, kiddogescort), `(lambda ((arg0 object) (arg1 <bot>)) ...)` stored as
  `check-done` / `(function <wait-spot> <bot> symbol)`: arg0 unused in all of them; 2x
  `leg-ik-callback` (arg1/arg2 `object`, unused); `pilot-states.gc` rotate-surface hook
  (arg0-2 `object`, unused; Jak 1 retyped its twin to `surface`).

Not checked statically: casts from untyped values (`function`, `basic` fields). The ABI check
mode run (Oct 18, below) covered the title and the slums.

## C mode on the Mac (Oct 18)

(AI-assisted.) Strict C build into `port_test/j2mirror`, gk from `<worktree>/build`.

Bugs fixed to boot:
- **Symbol values (goalc C backend).** The generated C read and wrote a symbol's value at the
  symbol's own st offset, which is only right for Jak 1. In Jak 2 the value is at the symbol
  address - 1 (Symbol4.h; the native backends use `LINK_SYM_NO_OFFSET_FLAG`). The first boot
  crashed in gcommon's top level. `IR_C.cpp` now emits `GC_SYMOFF(k) - 1` for Jak 2+; Jak 1's
  generated code and module hashes are unchanged. After this fix C mode booted with no kernel
  changes: the Jak 2 goalc_kernel (suspend/resume with the Jak 2 offsets, set-to-run-bootstrap,
  catch/throw, reset-stack-and-call) worked as written.
- **Kernel function argument widths.** New audit (commands above): gk writes every registered
  kernel function and the arguments its adapter reads in full; goalc checks them against the GOAL
  types. 2 of 177 read the upper half of 32-bit arguments: `file-stream-open` (`kopen`) and
  `dgo-load`. Both are `u32` now, like Jak 1's (it was harmless: `Ptr` truncates).
- **Jak2TypeConsistency** failed after the jak2-abi merge: `decompiler/config/jak2/all-types.gc`
  now has its speech-method and `target-warp-out` types. Jak3TypeConsistency failed on batch-oct11
  already (`SMALL_MEMORY` undefined in the shared jak1/pc files): Jak 3's gcommon defines it `#f`.

Runs (all silent, `--null-gfx` at 30 Hz unless noted):
- Title, 5 min idle: stable, 1800 frames per minute, Jak in `target-title` throughout.
- `jak2_city.pad`: ctysluma-start in 18 frames; memory report: global heap 52.0 of 62.9 MB used
  (native 55.3), scheme-globals 5508 KB (native 10115: C mode keeps no code in the GOAL heap),
  ctywide's level heap 3402 of 4354 KB (native 4196: its 1240 KB of arm64 code is gone), 291
  processes incl. 45 riders, 10 guards, 45 citizens, 55 vehicles. Jak ends at (1065.99, 0.52,
  194.46) m in `target-hide`, exactly as native.
- OpenGL (`OPENGOAL_GL_NO_VSYNC=1`, screen locked): title (frame 900) and the slums (standing and
  after walking) look the same as native; Jak's position matches to the cm.
- ABI check mode (`-DGOALC_ABI_CHECK`, city pad): 40 reports, all expected: `enter-state` /
  `set-to-run` / `run-function-in-process` / `__goalc-reset-stack-and-call` WIDE arguments beyond
  the caller's count (as in Jak 1); `format` arguments beyond the format string's (rpc-h's
  "flushing RPC port" warning); `sparticle-track-sun` arg0 (unused); pointer arguments with the
  check marker in pc-settings `read-from-file` (the `new 'stack` allocation comes back through a
  narrow parameter; normal builds zero-extend it); `kopen` (fixed above).
- Stack poison (`-DGOALC_STACK_POISON`, city pad): no difference, same end position and state.
- Not seen: any `[goalc]` error, suspended-stack overflow, or NaN symptom.

Timing (Mac, C mode, null renderer at 30 Hz, logic = kernel dispatch minus vsync/render waits,
averaged over the pad windows): Haven City slums standing in traffic **0.83 ms/frame**, walking
0.87 ms. Same session, Jak 1 (host small-memory gk, proj-final): village1 0.20 ms, Snowy 0.14 ms.
The Jak 1 numbers are at the perf lines' 0.1 ms resolution, so the ratio is rough: the city costs
about 4-6x Jak 1's village1. With Jak 1's Mac-to-N3DS ratio (village1 ~12.5 ms game thread on
N3DS, Snowy 11-13 ms logic on hardware: ~60-85x), Haven City logic would be roughly **50-70 ms on
the N3DS** (15-20 fps from logic alone) before any 3DS-specific work. Measure on the 3DS (a
logic-only boot) before planning around this; the city's traffic (citizens, vehicles, nav) is the
first profiling target.

## Next steps

Milestone 1 (Mac): done except as noted.
1. Done: strict C build, C-mode boot, title and city stable, OpenGL matches native, ABI check and
   stack poison runs (above).
2. Done: Haven City profile (docs/3ds-port/jak2_perf.md: sampling profiler, every district, the
   N3DS estimate and the <= 15 ms plan). The 30 Hz Mac logic numbers below are unreliable (the
   game thread runs on efficiency cores); use the no-sync windows described there.
3. Done (jak2-route, native; C mode numbers above). Next: choose the Jak 2 small-memory layout.
4. Not done: Jak 2 C-mode runtime tests in goalc-test (Jak 1 has `GoalcRuntime*` and C-mode
   kernel tests; Jak 2 has only compiler tests).
5. The C module hazard is reachable in Jak 2 (risks below): fix it before the 3DS build relies on
   the city.

Milestone 2 (boot on the 3DS) and the baseline the user asked for (Oct 12):
- Unmodified Jak II logic: crash fixes only, plus mandatory porting (C-mode kernel, 3DS platform
  layer, heap sizes to fit 124 MB, SD I/O for the overlord). No gameplay or performance cuts.
- First a logic-only boot with the null renderer, which gives a real Haven City frame time and
  gk.log perf lines. Then as much of the presentation as possible, reusing the Jak 1 Ctr renderers
  adapted to Jak 2's buckets: background (tfrag/tie/shrub via the converter on Jak 2 fr3), merc incl.
  envmap, sprites/particles, HUD/text, shadows, eyes, sky, ocean, texture animation (CPU fallback
  OK), screen effects, and sound/music/streaming (Jak 1 ndsp mixer path).
- Separate CIA/title: its own title ID, data in `sdmc:/3ds/jak2/`, a jak2 `config.ini` whose
  switches default to the unmodified behaviour. Staging in `port_test/3ds-sdcard-jak2-baseline`
  (intermediate baselines `-a`, `-b`). Before staging: check_svc, check_shaders, a silent host run,
  and a silent run in a muted private emulator instance (not emu-cell).
- 3DS build work: `platform/3ds/CMakeLists.txt` lists only Jak 1 kernel sources. Jak 2 needs its
  kernel, overlord (jak2) and mips2c sources, plus `SMALL_MEMORY` for Jak 2 (gcommon.gc has it at
  #f now; END_OF_MEMORY, heap sizes, `*goal-small-memory*` check).
- Renderer work that can go to parallel agents once the base boots: (1) background via
  converter + CtrLevel for Jak 2 fr3, (2) merc/eyes/envmap/shadow on Jak 2 buckets, (3)
  sprites/particles/HUD/text, (4) sky/ocean/texture animation/screen effects, (5) sound/overlord
  streaming. Boundaries: game/graphics/ctr/* per renderer, decompiler/level_extractor + converter,
  game/overlord/jak2 + game/sound.

## Open questions and risks

- Speed: rough Mac estimate above (city logic ~4-6x Jak 1's village1, maybe 50-70 ms on the
  N3DS). Unknown until a logic-only 3DS boot.
- Memory: Jak 2 PC uses `BIG_MEMORY`. The PS2 levels were sized for 32 MB, and the city keeps
  several large levels plus `*city-dead-pool*` in memory.
- `-boot -debug` crashes natively on arm64. This may hide a real bug that C mode also hits.
- (Fixed Oct 18 with module instances, see milestone 2.) The C module hazard (c_backend.md): two
  loaded levels with the same object file share one seg_base, so the first copy's code uses the second copy's static data (and freed memory once it
  unloads). Jak 2's DGO lists (Oct 18): 122 objects are in more than one DGO; the ones that can
  meet in the city: `searchlight` (cwi = ctywide, always loaded in the city, and pac), and the bot
  code `ai-task-h`, `bot`, `bot-h`, `bot-states` (ate, ctyasha, ctykora, lkiddoge, seb, swb, unb),
  `hal`, `hal-h`, `hal-task` (ctykora, lkiddoge, seb, swb, unb), `ash*`, `juicer` (ate, ctyasha).
  Whether two of those are ever loaded together (e.g. ctykora with lkiddoge, ctywide with pac)
  is not checked. Safest: per-link segment bases (c_backend.md has the design).
- mips2c: Jak 2 has 28 mips2c files (`game/mips2c/jak2_functions`) and no native ARM versions.
  The `(method 33 sky-work)` mips2c function is registered twice (warning at boot).
- `jak1/goalc_kernel.cpp` now includes the hook. The 3DS Jak 1 build was not rebuilt from this
  branch to confirm it still links. It only uses a pointer defined in that file, so it should.
  (The host checks on Oct 18 passed: Jak 1 level_pairs.pad on the host small-memory gk, exit 0,
  19 loads; goalc-test all pass.)

## CIA art (for the baseline package)

- Icon: `port_test/cia-art/jak2/icon.png` (48x48, the user's Jak + Daxter art, a top-left 412x412
  crop of `icon-source.png`). Use it as-is.
- Banner: render from Jak II's own title logo model, like Jak 1 (memory note
  3ds-banner-icon.md): logo only on a transparent background, with
  `platform/3ds/tools/make_banner.py` from the exported logo model. Do the export in a scratch
  mirror without touching jak-project/out or decompiler_out.
- Jingle: not chosen yet. Pick a short iconic Jak II sound effect (Jak 1 used the power cell's
  "cell-prize"), say which, and the user can swap it.
- Keep the Jak II art in its own git-ignored folder (e.g. `platform/3ds/cia/local/jak2/`), so Jak
  1's banner and icon stay untouched.

## Scripted route and memory survey (jak2-route, Oct 18)

(AI-assisted.) Branch `jak2-route`. Native PC (arm64) only; C mode not tried.

### Route into Haven City: solved

Cause of the `target-title` loop: on the title screen the intro movie runs in a `scene-player`
("title-disk-intro"). When it ends (or sees a new Jak), it sends `*target*` to its end point, the
`title-movie-end` continue, whose `title-movie` flag puts Jak in `target-title`. `target-title`'s
enter then spawns a new `title-control`. Killing the title-control alone could not help.
`pad-script-continue` (`goal_src/jak2/pc/features/speedruns.gc`) now kills the scene-player and
the title-control, then starts. Jak stands in the city 20-46 frames after `continue` (85 for
Strip), from the title screen or from another continue point. Ideas (a), (b) and (d) were not
needed.

Second problem: **no traffic in the native PC city.** ctywide's level heap (small-center, PC
1.1x = 4178 KB) is full after loading: 4163 KB used, 1240 KB of it arm64 code. `ctywide-login`'s
32 KB `*city-dead-pool*` allocation then fails (`kmalloc: !alloc mem global-object`), the
traffic-manager can't be spawned ("*city-dead-pool* was empty"), and there are no citizens,
guards or cars. Commit 907ce45a9 sets `DEBUG_LEVEL_HEAP_MULT` to 1.15 (PC only). With it the
slums have about 290 live processes: 45 citizens, 10 guards, 45 riders, 55 vehicles. It is a
separate commit, so it can be dropped. C mode doesn't put code in the level heap, so the 3DS
doesn't need it.

Pads (`platform/3ds/tests`):
- `jak2_city.pad`: title, then `continue ctysluma-start`. Waits until Jak stands, then 400
  frames for traffic and a memory report. Timing windows `perf-mark city-stand` (600 frames
  standing in traffic) and `city-walk` (300 frames walking forward, ends against a wall),
  then exit at frame 2400.
- `jak2_route.pad`: the slums, port, market (`ctymarkb-burning-bush`), main town (`ctygenb-start`),
  industrial (`ctyinda-start`), farms (`ctyfarma-start`), then the sewer (`sewer-start`, a
  non-city level). Each stop: wait to stand, memory report, walk 200 frames. Exit at 5900.
  (`strip-start` works too, but walking forward there kills Jak.)

Run them with `--null-gfx` (`OPENGOAL_NULL_GFX_FPS=30` or 60). Windowed OpenGL runs stalled
before the first frame twice on Oct 18 (probably while the display was asleep); null-gfx runs
were fine. `print *pad-script-mem*` in any pad prints the memory report: heaps, pools, levels,
live processes by type, and global heap categories. For the categories, the Jak 2 kernel's
`kmemopen`/`kmemclose` now keep totals (`game/kernel/jak2/kmalloc.cpp`, printed through
`pad-script-kmem-report`).

### Memory on native PC (arm64, level heap 1.15x)

Global heap: 61.5 MB, 55.3 MB used (the same in every spot: everything big is preallocated).
By kmemopen category:

| Category | PC KB | PC multiplier | PS2-size estimate |
|---|---|---|---|
| level-heaps | 21024 | 1.15x (1.1x upstream: 20148) | 18396 (146 pages x 126 KB) |
| scheme-globals (kernel + GAME.CGO code and data) | 10115 | arm64 code included | less in C mode (no code) |
| dma-buffers | 10265 | 4x (`PROCESS_HEAP_MULT`) | ~2600 |
| process-buffers (`*nk-dead-pool*` 6160 + fixed pools) | 6400 | 4x | ~1780 |
| part-systems | 3693 | 12x (`SPRITE_MAX_AMOUNT_MULT`) | ~310 |
| sprite-tables | 3330 | 12x | ~280 |
| tpages | 879 | | 879 |
| other (anim-manager, collide lists, game-info, text, bigmap...) | ~930 | | ~930 |

Levels loaded at once:

| Spot | Levels (mode, heap used / size KB, code KB) | Live processes | nk pool used |
|---|---|---|---|
| title | title (s-edge 2637/6624), ctywide (s-center 4164/4178), lwidea (borrow 2088/2304), ctysluma (s-edge 5852/6624) | 161 | 222 KB |
| slums | ctysluma (5852/6912, 73), ctyindb (5949/6912, 12), ctywide (4196/4354, 1240), lwidea (2088/2409) | 291 | 918 KB |
| port | ctyport (5899, 54), ctypal (5783, 22), ctywide, lwidea | 288 | 979 KB |
| market | ctymarkb (5774, 49), ctyindb, ctywide, lwidea | 299 | 978 KB |
| main town | ctygenb (5525, 13), ctygena (5309, 27), ctywide, lwidea | 374 | 980 KB |
| industrial | ctyinda (5893, 12), ctyindb, ctywide, lwidea | 312 | 920 KB |
| farms | ctyfarma (5537, 69), ctygena, ctywide, lwidea | 346 | 1078 KB |
| sewer | sewer (large 10190/13824), sewerb (s-edge 5986/6912, 629) | 85 | 145 KB |
| strip | strip (large 11411/13824, 389), vinroom (s-edge 5574/6912, 114) | 150 | 282 KB |

The city fills the whole level heap: two districts take the edge sections (48 pages each), and
ctywide takes the centre (50 pages). lwidea (and a 380 KB second slot) is borrowed from
ctywide's top. District data is 5.3-5.95 MB per 6048 KB PS2 section, which fits at 1.0x. All
traffic processes (up to ~360) come from `*nk-dead-pool*`, 0.9-1.1 MB at peak. That is 60-70% of
the PS2 1540 KB pool, under its 768-process cap. `*city-dead-pool*` is only 32 KB, and only the
traffic-manager lives in it.

Renderer-side data (fr3, a proxy for the C heap / linear of the Ctr renderers): the slums set
(ctysluma 7.1 + ctyindb 5.5 + ctywide 2.5 + lwidea 4.7 MB) is 19.9 MB. The biggest district
pair (slumb + slumc + ctywide + lwidec) is 21.2 MB, and strip + vinroom is 21.4 MB. Jak 1's two
loaded levels are about 23-28 MB (village1 11.5, beach 11.6, citadel 16.3 MB). So the
renderer-side budget is about Jak 1's.

### What a small-memory (3DS) Jak 2 layout needs

The EE global heap, in C mode with PS2-like multipliers: level heap 18.4 MB at 1.0x (20.2 MB at
1.1x), DMA 2.6-5 MB (1-2x), process heap 1.8-3.3 MB (1-2x; 2x gives the city pool margin),
particles/sprites 0.6-2.4 MB (1-4x), tpages and the rest ~1.8 MB, plus engine data (at most
10.1 MB; less without code). That is **~35-43 MB of global heap**, so an EE of ~40-48 MB. Jak 1's
48 MB small layout (42.5 MB global heap) is the right size. Jak 2 needs its own END_OF_MEMORY /
`SMALL_MEMORY` constants in gcommon.gc and `PROCESS_HEAP_MULT` / `SPRITE_MAX_AMOUNT_MULT` /
`DEBUG_LEVEL_HEAP_MULT` small-memory values. Measure the first C-mode boot with this report
(scheme-globals without code) before choosing the multipliers. With the renderer data about Jak
1's, the 124 MB app region looks feasible at 48 EE + ~25 C heap + ~40 linear. The risk is the
C-mode binary size: Jak 2 has 840 modules compiled to C, against Jak 1's.

## Memory: dormant levels, stream arena (j2-mem, Oct 18)

(AI-assisted.) Branch `j2-mem` (streaming.md "Jak II: dormant levels and the arena size"). Azahar,
the CIA merged with j2-sound + j2-texanim (525b0c0acc), `jak2_route.pad`, the `[ctr] memory`
line after each stop's memory report (baseline B: slums linear 131 KB, heap ~3.8 MB, ctyindb
loaded incomplete):

| stop | linear free KB | heap free KB (largest block) | note |
|---|---|---|---|
| slums | 1839 | 2846 (2335) | ctyindb dormant (2.4 of 7.4 MB resident); its load was still incomplete (3.3 MB left out, now never repaired while dormant) |
| port | 7527 | 3176 (1971) | |
| market | 128 | 2511 (963) | ctyindb visible from the spawn point: not dormant |
| main town | 5466 | 2991 (1412) | ctygena dormant |
| industrial | 3371 | 2909 (951) | ctyindb dormant |
| farms | 1955 | 3377 (1556) | ctygena dormant |

Not reached: >= 3 MB linear and >= 6 MB heap everywhere. What holds them (heap trace build,
`OG3DS_HEAP_TRACE`, slums): EE 48 MB; CtrVram 4.1 MB; sound banks 1.9 MB (SFXBlock 4 x 373 KB,
music 382 KB, loaded with sound = off); 14 libctru thread stacks 1.3 MB; IOP ramdisk 811 KB
(Jak 1's InitRamdisk, unused by Jak II's vis) + paged memory 576 KB + 11 coroutine stacks 704 KB;
the levels' tables ~3.5 MB (time of day ~1.5 MB, chunks / draws / draw order / tie LOD / stream
tables); impostors ~0.9 MB; .c3l reader buffers 640 KB; log ring 256 KB; GlobalProfiler 152 KB.
Linear: ~6.5 MB is the renderer's own (command buffers, vertex rings, present buffers, command
list arena, merc double buffers); texture pools stay in linear behind their VRAM copies.
.text: -Os on the GOAL C modules saves 3.9% (80-module sample: 1058 -> 1017 KB), ~0.5 MB for all
of them; the 108 module instances (+1.7 MB) need per-instance code sharing (a codegen change) or a
proven shorter list.

## Memory: heap and linear cuts (j2-mem2, Oct 18)

(AI-assisted.) Branch `j2-mem2` from baseline C. Jak 1 keeps its sizes: everything below is behind
`OG3DS_GAME_JAK2` / `OPENGOAL_NO_JAK1` (the Jak II 3DS build) or the game version at run time.

| Cut | What | Saved |
|---|---|---|
| GS VRAM copy (`CtrVram`) | Kept per 16 KB chunk, made at the first write (unwritten chunks read as zeros, as before). Chunks only `relocate()` writes into PSMT8H / 4HH / 4HL (the fonts in the depth buffer's top bytes: 110 chunks) keep only the words' top bytes (4 KB). Jak II's uploads are lazy, so only what something reads is written: 1368 KB on the whole route, all of it during boot and the title | heap 2.7 MB |
| Jak 1 code in the Jak II build | `OPENGOAL_NO_JAK1`: Jak 1's kernel entry, overlord entry and global inits, mips2c table; the linker drops the rest (Jak 1's overlord pieces Jak II uses stay: ramdisk server, fake ISO, the C-mode kernel's suspend hook) | .text 783 KB, .data 29 KB, .bss 273 KB (heap +1.06 MB) |
| Impostors | Off for Jak II unless `impostors` is in config.ini: in the city they drew ~1000 of 75k triangles a frame as cards | heap ~0.9 MB, VRAM 576 KB (texture + depth) |
| RAMDISK | `jak1::InitRamdisk(false)`: Jak II's vis is never in the IOP (`update-vis!`: "ramdisk vis ... not supported") and nothing calls `*ramdisk-rpc*`; a load would fail like a full RAMDISK | heap 811 KB |
| IOP coroutine stacks | 16 KB each (was 64; 11 threads). Deepest on the route: 2.7 KB. `check_stacks` now stops the game with a log line if a stack is used up (it ran into libco's context) | heap 528 KB |
| Thread stacks | EE 128 KB (was 512: C-mode GOAL runs on its own stack in EE memory; EE used 3.9 KB), IOP 64, EE-Worker 64, DMP 48 (were 128), mixer 64 (was 128) | heap 656 KB |
| Command buffer | 1.25 MB (pipeline halves 640 KB, were 1 MB); route peak 277 KB in one frame (`commands (peak N of M KB)` in the render line) | linear 768 KB |
| Sound banks with sound off | No audio output (`sound = off`, no DSP): banks and music aren't loaded (the bank counts as loaded, its sounds don't start; the EE's voice streams don't need banks) | heap 1.9 MB in the city (only with sound off) |
| GlobalProfiler | Its event buffer only when profiling is switched on | heap 152 KB |
| Heap -> linear | Linear 34 MB (was 32): heap 71.1 MB at boot (72.1 before), still over the 68 MB check | linear +2 MB |

New logs: `[ctr] thread stacks used (KB used/size)` (libctru threads, from `ctr_thread_create`: their
stacks are filled with a pattern at start; every 10 s with the IOP's line when one went 4 KB
deeper), `VRAM copy N KB` in the `[ctr] memory` line, `commands (peak N of M KB)`.

**Azahar (one run: the CIA, `jak2_route.pad`, private instance, sound on, instance muted; staged
files compressed as on the card).** The `[ctr] memory` lines in each stop's window (standing, then
the 200-frame walk); before = the j2-mem table (Azahar, same route) and baseline C (slums):

| stop | linear free KB (largest) | heap free KB (largest) | before: linear / heap (largest) |
|---|---|---|---|
| slums (ctyindb dormant) | 5140 (2324), 4224 (2244) | 7828 (7601), 7529 (7233) | 2878 / 3295 (2778) baseline C; 1839 / 2846 (2335) j2-mem |
| port | 9506 (5820) | 7965 (6883) | 7527 / 3176 (1971) |
| market, standing | 8695 (5832) | 8376 (6202) | |
| market, ctyindb loaded and in view | **1804** (1632) | 6797 (5650) | 128 / 2511 (963) |
| main town (ctygena dormant) | 8328 (5788), 8797 (5836) | 7885 (6632), 7787 (6495) | 5466 / 2991 (1412) |
| industrial (ctyindb dormant) | 7550 (4300) | 7379 (5774) | 3371 / 2909 (951) |
| farms | 14527 (5836), 3512 (2244) | 9041 (6495), 7932 (6263) | 1955 / 3377 (1556) |
| sewer | 4913 (4756), 3377 (3216) | 6661 (5012), 6537 (5012) | (2.6 MB / largest 257 KB earlier) |

The thread stack sizes for EE and the mixer were set from this run's measurements afterwards
(EE 512 -> 128 KB, mixer 128 -> 64 KB: 448 KB more heap than the table shows; not run again).
No `[heap]` failures, no "making room" or incomplete loads; every load complete; slums 29.5-29.6
fps (logic 21-24 ms), render thread and GPU as baseline C; 0 DSP drops; 13 bank loads. Host
(`gk --ctr-gfx`, small-memory C mode, `jak2_route.pad`, `CTR_STREAM_CHECK=1`): the same stops,
positions (one walk end differs by 0.6 m: run-to-run, as noted in jak2_perf.md), dormant and stream
checks as the base build; decoded textures byte-identical to the base build (`CTR_TEX_DUMP`, city
pad, alpha and colour). Jak 1: host `level_pairs.pad` on proj-final gives the same pad output as
the base build (exit 0); the Jak 1 3DS build links and makes its CIA (its .text is 16 KB smaller:
the zero chunk isn't in it).

**Not reached:** >= 3 MB linear at the market while ctyindb is fully in view (1.8 MB; 4.1 MB once
it goes dormant, before only 128 KB with parts of it left out). Heap >= 6 MB everywhere (least 6.5
MB, sewer, before the stack cuts). Left: the time of day tables (0.3-0.9 MB per district; storing
only non-black palette entries or 16-bit stamps saves ~10-25% of them for real work in the hot
blend path), heap fragmentation from level and bank loads (largest block 5.0-7.6 MB now; the bank
buffers could get their own arena), Jak II's SPU memory (845 KB of .bss) and the .c3l reader's
640 KB.
