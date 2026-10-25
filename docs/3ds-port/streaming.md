# Geometry streaming (c3l v19)

(AI-assisted, Oct 17.) Format: [c3l_format.md](c3l_format.md) "Near chunks (v19)". Code:
`tools/ctr_level_converter/main.cpp` (near chunks), `game/graphics/ctr/CtrLevel.cpp` (the
streaming section, `CtrLevels::stream_*`, and `draw_level`), `game/graphics/ctr/CtrStream.cpp`
(the stream arena), `c3l_reader.cpp` (`preload`). Config: `streaming`, `stream_kb`,
`stream_reserve_kb` ([config_dev.md](config_dev.md)).

## Why

v17/v18 kept each tie LOD group's near version (geometry 0) with the level, in linear memory from
the level's load to its unload. Geometry 0 for every group needs up to ~17 MB per level (lavatube
16.7 MB, snow 8.4, firecanyon 8.0), so ten levels had a budget (`kTieLodBudgets`: citadel 1869 of
1977 groups, firecanyon 965/1026, jungle 780/792, lavatube 2874/5772, misty 1345/1696, ogre
758/1210, rolling 739/1359, swamp 1013/1119, village1 1722/1905, village2 792/1110) and the groups
left out stayed at geometry 3 everywhere. A near version is only drawn within its group's switch
distance of the camera (shape error x focal / tie_lod_px + radius: tens of meters for most groups),
so most of that memory held geometry nobody could see at the time.

v19 keeps everything else of a level resident as before (tfrag of all tiers, the far versions of
every tie group, shrubs, the vista tier, merc models, textures) and streams only the near
versions: it reads them from the SD card around the camera on the loader thread and frees them far
away. A near version that isn't there yet draws its group's far version (geometry 3), which is
always there: the same picture as a group beyond its switch distance, never a hole.

## Units, and why nothing goes missing

A **unit** is a near chunk (a CHUNK_KIND_TIE chunk with only kRunLodNear runs, StreamChunk). A
group's near version is in one unit, rarely two (a cell split at 65535 vertices); its far version
is in an ordinary tie chunk that is always loaded. Per frame (`draw_level`):

1. Each unit's distance decision (the chunk's own draw distance with its hysteresis, as every chunk
   has: `decide_dist`, shared with the chunk loop) and whether it is there: `unit_ok` 1 there and
   within its distance, 2 beyond it, 3 within it but not there (late), 0 far mode / never loaded.
2. Each tie LOD group's choice as before (screen-space error, hysteresis), then: near only if every
   unit holding its near version has `unit_ok` 1. Else far (statistics: late when one was 3).
3. The units are drawn when `unit_ok` is 1 (their runs: the near versions of the groups that chose
   near, as any tie chunk's LOD runs); the far chunks' runs as before.

**No pop when late data arrives.** A group drawn far *in view* because its near data was late
keeps its far version while it stays in view once the data is there (`lod_state` bit 2), at most
1 s (30 frames; 4 s before lodnear), and only while the switch would show but the far version isn't
plainly wrong: the camera within 2/3 of the group's error distance (there the far version's error
is >= 1.5 x tie_lod_px; beyond it the switch is near the size of an ordinary LOD switch) but not
within 1/3 of it (an error of 3 x tie_lod_px or more: near at once). Out of view it switches at once.
(lodnear: with the 4 s hold, the late groups after a continue stayed coarse 3-15 m from the camera
and then switched together.) The statistics count the frames
with groups kept so.

So a group's near runs are drawn only from a mesh that is there, and its far runs are hidden only
when it chose near, which needs all of its near meshes there and within their distance: there is no
frame where neither version is drawn. The rest of a unit's culling (the view test of its sphere,
visibility bits, run culling, fine_vis) is the same as for any tie chunk and doesn't depend on
streaming; a far version's triangles lie inside its near version's corners (geometry 3's corners
are geometry 0 vertices), so when a unit is out of view its groups' far versions are too. A level
seen from a neighbouring one (far mode) never draws units: its groups are far there (as far mode
drew before, minus their near versions). Host check: env `CTR_STREAM_CHECK=1` counts every frame's
groups drawn near without all of their data (`near versions drawn without their data`: must be 0),
the ones drawn far because their data was late, and compares the file's run spheres and
kDrawAlpha80 flags with what the load works out from the vertices.

## Where a unit is needed

A group g is near within `e_g x k + r_g` of its center (e_g its shape error, r_g its radius,
k = focal / tie_lod_px x detail_scale x 1.1: the hysteresis' 1.1; ~70 in gameplay).
(AI-assisted, lodnear, Oct 18) With the PS2 floor, e_g is max(shape error, PS2 far-plane x
detail_scale / k) (`CtrLevelData::lod_switch_error`), and a unit's bound is need_a + k x max(need_e,
need_d x detail_scale / k) (need_d: its groups' largest far-plane, made at load: no new file data). A
unit that one of its groups needs by shape error (the far version would show more than tie_lod_px)
is **urgent**: wanted urgent units go before the others (an extra kInViewMeters of priority), and
at the arena's capacity an urgent unit that still has no room frees units there only for the floor,
the furthest first (their groups go far, within tie_lod_px). A unit is
needed while that holds for one of its groups: its **score** is the smallest `|p - c_g| - (e_g x k +
r_g)` over its groups g, at the camera now and 2/3, 4/3 and 2 s ahead at its speed (velocity per
frame, smoothed, reset by cuts of more than 40 m), and at Jak (his first bone's place from the merc
renderer: world = camera position + camera rot x tmat[3], within 0.6 m of the game's position):
camera cuts (cutscene, race and zoom cameras) land near him. k is this frame's factor x 1.15 or
its recent largest (decaying 1% a frame): a zoom moves it. The unit's bound (need_a + need_e x k
from its sphere's center, StreamChunk) skips the per-group pass for the units beyond it, squared
distances first. Score < 0: needed now; < 20 m + half a second of the camera's travel: wanted
(read); > 45 m: may be freed for another. A needed unit in view goes before every other.
(AI-assisted, titlemove) On the title screen e_g is at least kTitleLodMin (100 m) / k
(`lod_min_e`; performance.md "Title flight pops"), in the units' bounds too.
**Camera cuts**: a jump of more than 40 m in a frame, or of the tie LOD factor by more than 30%
(another camera's focal length, a zoom); late times are counted as after a cut (within 2 s of one)
or not (speed, bandwidth, room).
**Idle fill**: while nothing is wanted, no job runs and no level loads, every 8 frames one small job
(at most 128 KB) reads the nearest units within 250 m of their bound into the arena's free room (no
freeing, no growth): more is there before a cut.

The converter puts groups of similar need distance into near chunks of their own (need classes:
< 30, < 60, < 120, < 240 m and beyond, cells 50, 50, 100, 100, 200 m): one large rock in a 100 m
cell of small details made the whole cell needed from 300 m away. Snowy's near data needed at once
(camera at each group's center) went from 6.4 MB on average and 9.2 MB at most (100 m cells) to
4.0 / 6.2 MB; village2 3.5 / 4.7 -> 2.5 / 3.6, lavatube 3.5 / 5.6 -> 2.1 / 3.4 (finer cells for all
groups, 50 m: 4.2 / 6.3 Snowy).

## Scheduling (render thread, `stream_update`, each frame)

- The units of the loaded levels are scored (a level not drawn for 3 frames wants nothing).
- Wanted units that aren't there, most urgent (lowest score) first, of the level of the most urgent
  one, go into one stream job: at most 256 KB of file bytes or 24 units (512 KB / 48 while more
  than 1 MB is needed now), plus the units next to them in the file (their spatial neighbours) that
  are within the keep margin, while there's room without freeing anything. Two jobs in flight.
  Loader order: loads of levels the game waits for (it asked `ready()` for them within the last
  second), stream jobs holding units needed now, the other level loads (the neighbouring level of
  a continue, prefetches), the other stream jobs. A stream job is short (a level load waits for one
  at most ~50-100 ms on hardware).
- Room: each unit gets its arena piece when it goes into the job (the render thread allocates);
  when the arena has none, the units furthest beyond their need (score > 40 m) are freed
  (`ctr_gpu_mesh_delete`: the piece comes back once the GPU is done with it) until it fits, else
  the arena grows (below), else the unit waits (statistics: no room).
- (AI-assisted, popin2, Oct 25) **Priority at capacity.** Each unit has a priority
  (`StreamUnit::prio`): the largest of its groups' `e_g x k / (distance to the group's surface)`
  from the camera now. 1 means the switch distance (tie_lod_px of error); a unit in view counts
  1.5x. Needed units of levels with shape errors are read in priority order, most first. When a
  needed unit still has no room after the steps above, it frees units whose priority is 1.5x lower
  than its own, least first, but not units taken within the last 30 frames.
  - Why: in the Oct 25 hardware log (swamp + village2, lavatube + village3), the near data needed
    now was larger than the arena most of the time (6-7 MB needed, 8 MB arena), and every unit
    there was urgent. A needed unit waited until the camera left another unit's area: 0.5-3.4 s on
    average, up to 10 s. Its groups stayed far meanwhile and then switched on screen up close.
  - Now a group left far for lack of room is one whose far version shows little. It comes in once
    it shows more than what is there.
- (AI-assisted, popin2) **Draining.** While the linear heap is below its floor, all of the
  arena's growth units are freed so the growth segments can go back to the heap. While that
  happens, `CtrStreamArena::alloc` takes no piece in a growth segment (`set_draining`).
  - The bug: units read meanwhile went into the growth segment and were freed the next frame, so
    the segment never emptied.
  - Hardware log, rolling + village2, 2.6 MB free under a 3 MB floor for minutes: 250-850 near
    chunks read and freed again per 15 s, needed ones late 0.4-1.7 s on average, up to 15 s.
- The loader (`run_stream_job`) reads each run of parts that lie together in the file (across gaps
  of up to 48 KB of vertices / 16 KB of indices) with one request per section
  (`c3l::Reader::preload`: the run's blocks into a window), then each part's indices and vertices
  straight into its arena piece (`ctr_gpu_mesh_begin_ext`), and for a unit read the first time its
  time of day groups from its indices. Reads use the level's block directory as read at load (no
  directory read per job).
- The render thread takes the meshes (`take_stream`, `stream_take_pending`) at the next frame,
  each one's time of day colors written first with the current blend (below), at most 16k vertices
  a frame (a chunk at least; the rest the next frame): the unit is there from that frame on.

## Levels not drawn yet, level starts (title3, Oct 22)

- **Hidden levels**: a level that is loaded but not drawn in any mode for 3 frames (Sandover behind
  the title's logo for seconds; a level the game shows later) scores its units around the game's
  camera (`*math-camera*` trans, read on the game thread at `ctr_send_chain`:
  `CtrLevels::note_game_camera`), with its speed's lead, and the tie LOD factor of the last streaming
  level drawn or else of any level drawn (`note_k_any`: the title's logo). Its near chunks are there
  when it is shown. Before: none was read until Sandover's first draw, which drew 71 near chunks'
  groups far (Azahar 326 ms on average, 401 ms at most) and switched them on screen (the user's
  "geometry changes in the first couple seconds" of the title).
- (AI-assisted, todlag, Oct 23) **Time of day at a reveal**: a level shown again rewrites all of
  its groups drawn in its first frame (title3's spread over 6 frames at up to 7 ms drew groups
  with the file's colors meanwhile: removed). Sandover at the title: 20.6 ms of time of day in that
  frame in Azahar, behind the logo (c3l_format.md "Time of day").
- **Coming out of black**: no group is kept far "in view" for late data while the screen is black
  (`ctr_level_set_screen_black`: the game's PMODE fade at 0, or held). When the screen comes out of
  black within 3 s of a level being shown or of a camera cut, while tie LOD groups in view are
  drawn far for late data (`note_late_in_view`), the renderer keeps it black until they are there
  (at most 1.5 s), then fades in over 8 frames (`CtrRenderer::draw_pmode_fade`; log line `[ctr]
  screen kept black N ms`). A continue to firecanyon-start (host, SD model): 25 groups far at the
  first visible frame, 23 of them switching in view when the 4 s hold ended -> 11 frames more black,
  none far once visible.

## Memory

Linear memory: the level's resident data (everything but the units) is v16's tie data plus v18's
other improvements: per level the v18 near data less its share of the far versions' vertices. The
units live in the **stream arena** (`CtrStreamArena`): a first segment of `stream_kb` (4096 KB)
taken at the first level that streams and kept (one block: the units' comings and goings never
fragment the linear heap that level loads need whole), and **growth segments** (stream-arena
branch, Oct 17: up to the linear heap's floor, `stream_reserve_kb` 3 MB):

- **Growth**: when a unit wanted doesn't fit and freeing the units beyond the keep margin (45 m)
  didn't make room, one segment of what the camera needs now and 2 s ahead (`need_now`: the units
  with score < 0 at the four lead positions and Jak, the ones being read included) plus 1 MB (or
  1/8 of it) beyond the arena's capacity: at least 1 MB (or the unit), at most 4 MB, at most what
  keeps the floor + 256 KB free, halved while the heap has no block that big. So the arena takes
  the free linear memory down to ~3.25 MB in one to three large segments (before: 1 MB steps
  while 3.5 MB + 1 MB stayed free, whose unused tails left the arena's free room in pieces).
- **At capacity**: a unit needed now (score < 0) that still doesn't fit frees units kept within
  the keep margin that aren't wanted either (beyond the wanted margin), the furthest first.
- **Given back**: under the floor + 128 KB the units in growth segments not needed now, under the
  floor all of them; while a level loads (it asked for a level: the load's memory first) all of
  them unless the heap keeps the floor + 12 MB, and no growth then unless it keeps that much
  after it; when a level load runs out of memory (`make_room` -> `housekeeping` ->
  `stream_shrink(all)`); while an incomplete level waits for a repair, its missing bytes + 256 KB
  are added to the floor. Empty growth segments are freed at the next frame (their meshes' memory
  comes back once the GPU is done with it).

Heap (malloc): per level the unit table (32 bytes per unit),
the groups' unit lists, the time of day groups reserved for the units (12 bytes per visibility id),
the file's block directory (16 bytes per block: village1 ~22 KB, lavatube ~50 KB).

## Time of day, tie LOD, the other features

- Time of day (c3l v16): a unit is colored when it is taken, before its first draw: every group
  written from the table with the current blend (exact; ~80 ns per vertex in Azahar, within the take
  budget), its versions set, so `tod_recolor` treats it as up to date: never drawn with the file's
  baked colors. (A unit taken before its level's first draw keeps the file's colors until then:
  since Oct 23 tod_recolor rewrites every group drawn before its draw, no cap.) The groups (one per visibility id, v18) are
  made from the unit's indices by the stream job at its first read, into room kept at load; its
  light lists and palettes (`tod_scan`) at its first take, once per level load. Host check:
  `CTR_TOD_CHECK=1`.
- Tie LOD: unchanged for the far chunks; a unit's runs are LOD runs like any tie LOD chunk's.
- static_cmdlists: a recorded stretch is keyed by the draws' physical vertex and index addresses,
  counts and states: a unit freed and another read into the same arena piece with the same counts
  writes the same command words (the data at those addresses is what the GPU reads), so nothing is
  replayed stale (`static_cmdlists = compare` checks).
- Impostors (merged Oct 17: cards from the tie LOD version on screen): units are never cards and
  don't restart the level's cards when their meshes come and go. A far chunk's card holds far
  versions only (its near versions are in units): while a unit is late its groups are far and the
  card shows what is drawn; once they switch near their far runs are hidden, the card's cluster
  isn't shown and the unit draws them as geometry. No recapture needed.
- fine_vis: units are tie chunks (never occluders); a .c3v made from the v19 file covers them.
  Regenerate the .c3v files after the reconvert (`ctr_vis_builder --all <c3l folder>`).
- Partial unload (far mode), repair of incomplete loads: units are not theirs (never "missing").
- Older files (v7-v18) load whole as before; `streaming = off` with v19 files loads the core units
  with the level (v18's ties) and never the others.

## Logs

- Load: `[ctr] <level>: near chunks (v19): N with K KB (P KB in the file), streamed; the other
  chunks R KB`.
- Every stats period: `[ctr] streaming: arena U of C KB used (peak, largest free, pieces; growth G
  KB in S segments, at most M KB in all; taken, given back, units freed for it); J jobs, N near
  chunks read (KB, KB of their blocks; file KB in R reads, ranges in one request, ms reading, ms in
  jobs, longest; KB/s), freed, times no room; late: N times, F frames with groups drawn far for it
  (group frames), ms on average, at most (frames); needed now KB on average, at most; read with
  their file neighbours; scheduling ms/frame (most); per level: meshes KB + textures KB, near chunks
  there of all (KB)`.
- Host: `CTR_POP_LOG=1` (title3): one `[pop]` line per level draw (chunks appearing / going by
  distance in view, tie LOD switches in view, groups late / held far, time of day groups rewritten,
  left behind, waiting, impostor cards and captures, near chunks taken) and `[pop] soft frame N`
  after each dumped frame (`--ctr-dump DIR --ctr-dump-every 1`: frame_N.png).
- Host: `CTR_STREAM_SIM=5.5` takes each stream job only when a New 3DS SD card (5.5 MB/s, 8 ms per
  request) and decoder would have finished it, in frames of the fps cap (game time); `CTR_SOFT_LINEAR_KB`
  (default 34000) is the soft backend's model of the linear heap for the growth rule;
  `CTR_STREAM_CHECK=1` (above).
- Since stream-arena: the late line also gives the late times **for lack of room in the arena**
  (the arena had no room for the unit during that time, or it was freed under memory pressure);
  the arena line gives the **linear free least** KB at the frames' starts (and while a level
  loaded); the arena's peaks (used, capacity) are per stats period.
- `platform/3ds/tests/stream_traverse.pad`: the title's flight, a run through Sandover, the Zoomer
  in Fire Canyon.

## Measurements (Oct 17, Azahar New 3DS, private instance, sound off; host for the checks)

**Tie groups with their near version (geometry 0)**, v18 -> v19 (all candidates): citadel 1869 ->
1977, firecanyon 965 -> 1026, jungle 780 -> 792, lavatube 2874 -> 5772, misty 1345 -> 1696, ogre 758
-> 1210, rolling 739 -> 1359, swamp 1013 -> 1119, village1 1722 -> 1905, village2 792 -> 1110; the
other levels had every candidate already.

**Linear memory with level pairs** (`level_pairs.pad`, the `[ctr] textures` line's linear free
memory, least in the pair's window, KB; v19 counts the arena with its growth then):

| pair | v18 free | v18 largest | v19 free | v19 largest | v19 arena |
|---|---|---|---|---|---|
| lavatube + citadel | 4336 | 4200 | 11182 | 11048 | 4096 |
| finalboss + citadel | 12683 | 4600 | 15355 | 11152 | 4096 |
| maincave + robocave | 12748 | 12596 | 11094 | 10960 | 4096 |
| maincave + darkcave | 18691 | 12772 | 15411 | 11132 | 4096 |
| sunken + sunkenb | 16814 | 16680 | 19074 | 18920 | 4096 |
| village3 + ogre | 5914 | 5768 | 7044 | 6876 | 4096 |
| snow + village3 | 5223 | 5056 | 8284 | 7116 | 7168 |
| rolling + village2 | 5389 | 5256 | 5998 | 5852 | 4096 |
| ogre + village2 | 3832 | 3652 | 4440 | 4300 | 5120 |
| lavatube + village3 | 5405 | 5272 | 8871 | 8728 | 4096 |

Every pair keeps >= 4.3 MB (v18: 3.6 MB at worst) with every group at geometry 0 near the camera;
the maincave pairs lose 1.6-3.3 MB to the fixed arena (their levels have little near data). The
resident mesh data per level (vertex + index data, converter): v18 -> v19 other chunks, e.g.
village1 11578 -> 7126 KB, lavatube 16480 -> 10878, snow 16131 -> 7869, firecanyon 13569 -> 7457,
citadel 9824 -> 4503, ogre 15585 -> 12319.

**Level loads** (`[ctr] loaded`, ms, v18 -> v19): village1 3755 -> 2651, snow 4781 -> 2627, citadel
3305 -> 2032, lavatube 4207 -> 3008, ogre 4115 -> 3395, rolling 3837 -> 3371, village2 3396 -> 2957,
beach 4024 -> 2632, firecanyon 3812 -> 2124 (less read at the load).

**Fast traversal** (`stream_traverse.pad`): the title's flight over Sandover, the run from the hut,
the Zoomer in Fire Canyon. Late = a near chunk needed (one of its groups within its near distance)
before it was there: its groups drawn far meanwhile.

| segment | late after a cut (avg / max ms) | late otherwise (avg / max ms) | read from the SD card | busiest 15 s |
|---|---|---|---|---|
| boot + title start (no camera yet) | - | 70 (348 / 433) | 4.2 MB in 168 reads | 280 KB/s |
| title flight (~70 s) | 0 | 0 | 2.9 MB in 162 reads | 109 KB/s |
| Sandover hut continue + run | 7 (1688 / 3206) | 0 | 0.9 MB in 42 reads | 60 KB/s |
| Fire Canyon, Keira scene + Zoomer race | 41 (442 / 1236) | 46 (358 / 1135) | 12.5 MB in 602 reads | 494 KB/s |

Before the tuning (first working version): Fire Canyon ~150 late a run, 0.7 s average, 2.3 s at
most; the hut continue 44 at ~3.6 s (arena fragmentation and no growth while the beach loaded). In
view, none of these arrivals pops: the groups stay far until out of view (above). The late times
after cuts (the Keira scene's and the race's cameras, focal length changes) are what the scheduler
can't foresee; the others are the race at Zoomer speed. Host with the SD model (CTR_STREAM_SIM=5.5): the
Zoomer race 0.1-0.36 s average.

**Render thread** (`render ms/frame` build, v18 -> v19, same segments): title 19.32 -> 16.73,
Sandover run 14.90 -> 15.15, Fire Canyon 12.01 -> 12.72; of it the scheduler 0.45-0.54 ms/frame
(scoring every unit's bound, the groups of the near ones). GPU busy 20.15 -> 17.59, 16.20 -> 16.49,
13.37 -> 14.00 ms. Triangles 108640 -> 112422 (title), 85383 -> 83837, 74939 -> 76141: more groups
at full detail; within run-to-run noise elsewhere.

**Checks**: host `CTR_STREAM_CHECK=1` over level_pairs.pad, playthrough_sweep.pad, zoomer.pad and
stream_traverse.pad: 0 near versions drawn without their data, 0 near chunks drawn with the file's
baked colors, run spheres and kDrawAlpha80 flags of the file identical to the ones made at load
(166k spheres, 48k draws). Azahar `static_cmdlists = compare` over level_pairs.pad: 0 mismatched
copies (~10k stretches checked).

**Playthrough sweep** (host, all 68 checkpoints, CTR_STREAM_CHECK + SD model): passes (exit 0), 0
near versions without their data, 0 near chunks with baked colors. Late times: 1377 after cuts
(continues: 0.97 s on average), 1073 otherwise (0.48 s). The long ones (up to 27 s) are in Boggy
Swamp and Snowy Mountain, where the near data needed at once (5.6-6.6 MB) passes the arena and the
growth the host's linear heap model allows (CTR_SOFT_LINEAR_KB 34000 with two levels in): the
nearest chunks are read, the others' groups stay far (no holes) until room comes.

**Arena up to the floor** (stream-arena, Oct 17; Azahar, private instance, sound off, the user's
config, stats_period 5; per checkpoint of fps60_sweep.pad: continue, ~15 s camera turn, ~18 s walk;
before = 1 MB steps while 3.5 MB + 1 MB stayed free):

| checkpoint | arena MB before / after | needed now at most MB | late for lack of room (n / max ms) before / after | linear free least MB before / after |
|---|---|---|---|---|
| swamp-start | 9.0 / 9.4 | 6.3 | 0 / 0 | 3.83 / 3.35 |
| swamp-dock1 | 9.0 / 9.4 | 6.1-6.4 | 17 / 768 -> 15 / 701 | 3.78 / 3.35 |
| swamp-dock2 | 9.0 / 9.4 | 6.9-7.1 | 0 -> 3 / 5147 | 3.77 / 3.37 |
| snow-start | 9.0 / 10.0 | 6.0-6.2 | 0 / 0 | 3.78 / 3.37 |
| snow-fort | 11.0 / 11.8 | 6.3 | 36 / 935 -> 20 / 434 | 4.14 / 3.36 |
| snow-flut-flut | 11.0 / 11.8 | 4.5-6.1 | 4 / 200 -> 4 / 200 | 4.14 / 3.36 |
| snow-by-ice-lake-alt | 11.0 / 11.8 | 6.0-7.9 | 0 / 0 | 4.12 / 3.36 |

On Azahar (the real 40 MB linear heap, as on hardware) the growth already reached 9-11 MB before:
the near data needed at once (6-8 MB) fits; what is left is the arena's free room in small pieces
(swamp-dock2: 2.7 MB free, none over 222 KB: one 5 s late of 3 units). The late times are almost
all after the continue's cut. The host's linear model (CTR_SOFT_LINEAR_KB 34000) now lets the arena
to 7.2 MB in Swamp, 8-9 MB in Snowy: late for lack of room at most 0.7 s there (before: up to 27 s).
Level pairs (`level_pairs.pad` in the same run): least linear free per pair (KB; while a level
loaded): lavatube+citadel 11024 (11048), finalboss+citadel 15198, maincave+robocave 10862,
maincave+darkcave 10862, sunken+sunkenb 19022, village3+ogre 6761 (7450), snow+village3 6987
(arena 8.3 MB), rolling+village2 5817 (6032), ogre+village2 4259 (5681; arena 5.1 MB),
lavatube+village3 8855: every pair >= 4.2 MB, every load >= 5.6 MB. Host checks (CTR_STREAM_CHECK,
SD model) over level_pairs.pad and these checkpoints: 0 near versions without their data, 0 baked
colors, no load failures.

## Left / risks

- Capacity: where more near data is needed at once than the arena can hold (Swamp, Snowy:
  converter estimate up to 6.4 / 6.2 MB), the furthest needed groups stay far. On hardware the
  growth follows the real heap (more room than the host model); a level-dependent stream_kb or a
  memory-pressure factor on the near distances (applied out of view only) would make it orderly.
- Cuts: a cut to a camera with another focal length (cutscenes, the race camera) can't be
  foreseen; the near chunks around Jak and the idle fill cover most of it, the rest is drawn far
  for 0.3-1.7 s (never popping in view).
- The scheduler costs 0.45-0.55 ms of render thread time per frame in Azahar (all units' bounds
  every frame): could run every second frame.
- tfrag tier 1 and the shrubs are not streamed (tier 2 has holes up close: no coarse fallback that
  hides nothing); textures not streamed. Hardware SD timing not measured (Azahar reads the host's
  disk; the cost model is 5.5 MB/s + 8 ms per request).
- .c3v files: regenerate with `ctr_vis_builder --all` on the v19 folder (checked on sunkenb).
