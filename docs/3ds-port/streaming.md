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
k = focal / tie_lod_px x detail_scale x 1.1: the hysteresis' 1.1; ~70 in gameplay). A unit is
needed while that holds for one of its groups: its **score** is the smallest `|p - c_g| - (e_g x k +
r_g)` over its groups g, at three camera positions p: now, and 0.6 s and 1.2 s ahead at the
camera's speed (frames: velocity per frame, smoothed, reset by cuts of more than 40 m). The unit's
bound (need_a + need_e x k from its sphere's center, StreamChunk) skips the per-group pass for the
units beyond it. Score < 0: needed now; < 15 m: wanted (read); > 40 m: may be freed for another.

The converter puts groups of similar need distance into near chunks of their own (need classes:
< 30, < 60, < 120, < 240 m and beyond, cells 50, 50, 100, 100, 200 m): one large rock in a 100 m
cell of small details made the whole cell needed from 300 m away. Snowy's near data needed at once
(camera at each group's center) went from 6.4 MB on average and 9.2 MB at most (100 m cells) to
4.0 / 6.2 MB; village2 3.5 / 4.7 -> 2.5 / 3.6, lavatube 3.5 / 5.6 -> 2.1 / 3.4 (finer cells for all
groups, 50 m: 4.2 / 6.3 Snowy).

## Scheduling (render thread, `stream_update`, each frame)

- The units of the loaded levels are scored (a level not drawn for 3 frames wants nothing).
- Wanted units that aren't there, most urgent (lowest score) first, of the level of the most urgent
  one, go into one stream job: at most 256 KB of file bytes or 24 units, plus the units next to
  them in the file (their spatial neighbours) that are within the keep margin, while there's room
  without freeing anything. One job at a time; level loads go first on the loader thread (a stream
  job waits for them and is short: a level load waits for one at most ~50-100 ms on hardware).
- Room: each unit gets its arena piece when it goes into the job (the render thread allocates);
  when the arena has none, the units furthest beyond their need (score > 40 m) are freed
  (`ctr_gpu_mesh_delete`: the piece comes back once the GPU is done with it) until it fits, else
  the arena grows (below), else the unit waits (statistics: no room).
- The loader (`run_stream_job`) reads each run of parts that lie together in the file (across gaps
  of up to 48 KB of vertices / 16 KB of indices) with one request per section
  (`c3l::Reader::preload`: the run's blocks into a window), then each part's indices and vertices
  straight into its arena piece (`ctr_gpu_mesh_begin_ext`), and for a unit read the first time its
  time of day groups from its indices. Reads use the level's block directory as read at load (no
  directory read per job).
- The render thread takes the meshes (`take_stream`) at the next frame: the unit is there from that
  frame on.

## Memory

Linear memory: the level's resident data (everything but the units) is v16's tie data plus v18's
other improvements: per level the v18 near data less its share of the far versions' vertices. The
units live in the **stream arena** (`CtrStreamArena`): a first segment of `stream_kb` (4096 KB)
taken at the first level that streams and kept (one block: the units' comings and goings never
fragment the linear heap that level loads need whole), and **growth segments** (1 MB or a unit's
size) taken while every unit there is needed and the linear heap keeps `stream_reserve_kb` (3.5 MB)
free after it. Growth goes back when the heap has less than the reserve (units not needed now
first, all of them below the reserve - 512 KB), while a level loads (its memory first), and when a
level load runs out of memory (`make_room` -> `housekeeping` -> `stream_shrink(all)`); empty growth
segments are freed at the next frame. Heap (malloc): per level the unit table (32 bytes per unit),
the groups' unit lists, the time of day groups reserved for the units (12 bytes per visibility id),
the file's block directory (16 bytes per block: village1 ~22 KB, lavatube ~50 KB).

## Time of day, tie LOD, the other features

- Time of day (c3l v16): a unit's colors are the file's when it is read; `tod_recolor` sees a new
  mesh and rewrites each of its groups the first time it is drawn, at once (a group with the file's
  colors never waits for the budget): exact for the current time. The groups (one per visibility
  id, v18) are made from the unit's indices by the stream job at its first read, into room kept at
  load; the light lists and palettes (`tod_scan`) at its first draw, once per level load.
- Tie LOD: unchanged for the far chunks; a unit's runs are LOD runs like any tie LOD chunk's.
- static_cmdlists: a recorded stretch is keyed by the draws' physical vertex and index addresses,
  counts and states: a unit freed and another read into the same arena piece with the same counts
  writes the same command words (the data at those addresses is what the GPU reads), so nothing is
  replayed stale (`static_cmdlists = compare` checks).
- Impostors: units are never cards (cards show far versions) and don't restart the level's cards
  when their meshes come and go.
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
- Host: `CTR_STREAM_SIM=5.5` takes each stream job only when a New 3DS SD card (5.5 MB/s, 8 ms per
  request) and decoder would have finished it, in frames of the fps cap (game time); `CTR_SOFT_LINEAR_KB`
  (default 34000) is the soft backend's model of the linear heap for the growth rule;
  `CTR_STREAM_CHECK=1` (above).
- `platform/3ds/tests/stream_traverse.pad`: the title's flight, a run through Sandover, the Zoomer
  in Fire Canyon.

## Measurements

(filled in below)
