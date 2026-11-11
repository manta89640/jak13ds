# Not sending geometry that never reaches the screen (AI-assisted, Nov 11)

(AI-assisted.) Ten ways of keeping level triangles that can't show from the GPU, one commit each.
What a title flight frame sent before (performance.md "What the GPU time buys", host soft
renderer): of ~125k level triangles after the LOD work, ~42k behind nearer geometry, ~33k without a
pixel center, many back faces of double-sided draws, ~4% off screen.

Status of all of them: written and checked on the host (unit tests in `test/ctr_render`, the
renderer and converter build), **not yet run in Azahar or on a console**. Every one is behind its
own config.ini key and off by default, so a build without them in config.ini draws as before. The
ones with converter data need the levels converted again.

| # | key | removes | needs |
|---|---|---|---|
| 1 | `occluders` | runs and chunks behind big solid objects | reconverted levels |
| 2 | `horizon` | runs and chunks behind hills | reconverted levels |
| 3 | `title_vis` | on the title flight, every run the camera there never shows | a host bake (`.c3b`) |
| 4 | `depth_reuse` | runs and chunks a recent frame's depth hid, while the camera holds still | nothing |
| 5 | `cell_vis` | runs never seen from the camera's cell looking that way | a host bake (`.c3c`) |
| 6 | `ps2_lod_scale` | detail the 240-line screen can't show (coarse versions nearer) | nothing |
| 7 | `min_run_px` | runs and chunks under a pixel or so across | nothing |
| 8 | `far_copy` | detail of far plain ties (a simplified copy from 150 m) | levels converted with `--far-copies` |

## 1. Occluders: boxes inside closed tie surfaces

**Data** (`tools/ctr_level_converter/occluders.cpp`, `c3l::ExtOcc`). The converter already finds
closed tie surfaces for back face culling (`orient_ties`: welded, consistently wound, open only at
small holes or on the ground, a clear inside). For each one without planar openings (a door, a
window), no wind and no prototype the game hides, at least 1.1 m across on every side:

- the inside on a voxel grid (24 along the longest side, at least 0.2 m), by the parity of the
  crossings along rows, for six yaw angles (boxes stay upright, y up);
- from the voxel deepest inside, a box grown one layer at a time on each side while the layer is
  all inside; its faces at the outer voxels' centers;
- checked exactly: no triangle of the surface touches it (separating axis test) and its center is
  inside (rays in three directions); else shrunk by 15% (up to 8 times);
- the largest of the six angles. Written shrunk by the surface's morph offsets and the shape error
  of the tie LOD groups around it (inside every version), at least 0.5 m half extent and 6 m^2 of
  h0 h1 + h1 h2 + h2 h0, one box per node and place, the biggest 1024.

`--no-occluders` leaves the table out. The log has
`<level>: occluders: N boxes (of N surfaces big enough, N found)`.

**Why it is exact.** Every ray from the camera into the box crosses the surface first: the box is
inside the closed surface (and the surface's small holes and ground openings are covered by the
terrain or need two holes in a line). So a sphere all of whose points lie behind the box, seen
from the camera, can't show. `CtrCull.h make_shadow` builds that region from the box and the
camera: the faces turned to the camera (the region is beyond each) and the planes through the
camera and the silhouette edges (5 to 9 planes). A sphere is hidden when its center is at least its
radius inside every plane. The test (`cull_test.cpp`) checks random boxes, cameras and spheres:
every point of every hidden sphere has the box between it and the camera.

**When an occluder counts** (`choose_occluders`, per level draw):
- its node's bit in the game's visibility string (when there is one) is set;
- the ordinary tie chunks with its node's runs around it (`make_occ_support` at the first draw: up
  to four, not the near chunks or the vista tier) were all drawn opaque in the main pass the frame
  before (`chunk_opaque_frame`): not cut by a distance limit, not fading, not replaced by the vista
  copies. Either LOD version may be the one drawn: the box is inside both;
- in view, within the draw distance, the camera at least 3 m outside the box (nearer, the surface
  in front of it may be cut by the near plane);
- of those, the `occluder_count` (6) that look biggest (face areas / distance^2).

An occluder whose own surface is hidden by another one still counts: what is behind it is then
behind the other one too (the shadows are convex).

**What is hidden** (`emit_chunk`): a chunk whose sphere is in a shadow, before its run loop; else
the runs of the chunks that reach into a shadow, after the run loop (visibility bits, LOD, view
test), one by one by their spheres. Hidden runs can still be drawn with their neighbours by the
small-gap merge, as the view test's. Merc models are not tested.

**Cost.** Per level draw a pass over the level's occluders (a few hundred, a distance and a view
test each), the planes of the chosen ones, per chunk in view up to 6 sphere tests, per run of the
chunks reaching into a shadow up to 6 more. Memory: 64 bytes per occluder and 20 of support, read
only while `occluders` is on.

**Log:** `[ctr] <level>: occluders: N of M have the tie chunks that draw their surface` at the
first draw, then `[ctr] <level>: occluders: N chosen, hid N chunks and N runs (N tris) per level
draw` in each stats window.

**To check on a console:** convert the levels, `occluders = on`, play the beach, village1 and the
jungle; look for holes behind rocks and houses when they come into view and when the camera passes
close to them; compare `render ms/frame` and the triangle count with it off.

## 2. Horizon: what the terrain's hills hide

**Data** (`tools/ctr_level_converter/horizon.cpp`, `c3l::ExtHorizon`): the level's terrain as a grid
of 8 m cells. A cell is covered when its opaque tfrag (tiers 0-2: what a camera in the level may
see drawn) covers all of it, in every version present there (detailed and coarse tier, the
geomorph's versions), in one layer: 5 x 5 samples, each inside a triangle of each version, and the
non-steep triangles of a version at a sample within 2 m of each other (an overhang or a cave under
the ground is two layers). Per covered cell the lowest (`minh`) and highest point of the triangles
that reach it, morph offsets included. About 3 bytes per cell (village1 ~17 KB), read only while
`horizon` is on.

**Why it is exact.** Take the vertical half-plane through the camera towards a point. If every cell
along it is covered, the drawn terrain is a sheet across it that starts below the camera (the
camera is above its own cell's highest point). A ray in that half-plane that is below some cell's
`minh` within its column is below the drawn terrain there, so it crossed the terrain before. The
horizon (`CtrCull.h build_horizon`) keeps, per direction (128 azimuth bins), the highest elevation
angle below which that holds for the covered cells within `horizon_radius` (64 m):
- an uncovered cell (or the grid's edge) ends every bin it touches at its nearest distance;
- a covered cell within the radius raises the bins entirely inside its azimuth span, if they
  reach as far as it, to the lowest elevation of its `minh` over its distances.

A sphere entirely beyond the radius whose highest elevation (center's + its angular radius) is
below the horizon in every bin its azimuth span touches is hidden (`horizon_classify`); a chunk
partly below it has its runs tested.

The test (`cull_test.cpp`): a terrain with a 20 m ridge, random spheres behind it: every point of
every hidden sphere has the terrain between it and the camera (ray casts against the triangles); a
hole in the ground in front of the ridge leaves its directions alone; no horizon from under the
ridge's top or off the grid; an overhang's cells are not covered.

**Across levels.** The horizon is made once per frame (`frame_horizon`) from the first loaded level
drawn in near mode the frame before with the camera over a covered cell of its grid, and every
level draw of the frame uses it: the hills of the level the camera is in hide the far parts of the
other one too.

**What it can't do.** Only terrain makes the horizon (ties: occluders, section 1). One uncovered
cell near the camera (water drawn by the ocean, a gap, a blended or cut-out tfrag draw, a second
layer) ends the horizon in its whole wedge of directions.

**Cost.** Per frame: up to ~(2 x 9 + 1)^2 cells (64 m, 8 m cells), an atan2 and a few square
roots each, twice. Per chunk in view a horizontal distance test, and for those beyond the radius
an atan2 and two asin; the runs of chunks partly below it the same.

**Log:** `[ctr] <level>: terrain grid N x N cells of N m, N covered` at load,
`[ctr] <level>: horizon in N% of level draws, hid N chunks and N runs (N tris) per level draw`.

**To check on a console:** convert the levels, `horizon = on`, walk over village1's hills, the
beach and the jungle's paths, look for far things missing that should show over a crest, compare
the triangle count with it off.

## 3. The title flight's baked visibility

The title's camera flies one fixed path (the `logo-cam` animation; `kTitlePath` in CtrLevel.cpp,
already used to leave out chunks the path can never draw). So what it sees can be recorded once
and looked up: a sidecar `<level>.c3b` (`CtrVisBake.h`) next to the level's .c3l.

**Baking** (host only). The soft renderer tags each level triangle it draws with its level and run
(`ctr_gpu_soft_set_tagger`; a run from the chunk's draws and their runs' index ranges) and keeps,
per pixel, the tag of the last opaque level fragment, plus the tags of blended level fragments that
passed the depth test. At the frame's end those are the runs seen. With `CTR_VIS_BAKE=DIR:title`:
- a frame is recorded while the title is on and the camera is within 4 m of `kTitlePath`; it adds
  to the last sample, or starts a new one once the camera has moved 0.5 m or turned 2 degrees
  (sample: position, the four view planes' normals);
- every 900 recorded frames and when recording stops, `DIR/<level>.c3b` is written: per sample the
  runs seen, grown by every run whose sphere touches theirs (+1 m: the other LOD version, tier or
  near chunk of the same place, a vista copy over it); per run its samples grown by 2 on each
  side, as intervals. Runs without a sphere are seen everywhere.

Run the host renderer (`gk --ctr-gfx`, as for the other host checks) on the title with
`CTR_VIS_BAKE=DIR:title` for at least two loops of the flight, with the defaults (no new culling key
on), from the same .c3l files as the SD card, then copy `DIR/village1.c3b` (and any other level it
wrote) next to the .c3l files. The log has `[visbake] DIR/<level>.c3b: N samples, N of N runs seen,
N intervals (N KB)`.

**At runtime** (`title_vis = on`, `pick_vis_samples`): while the title is on, the two samples
nearest the camera within 2 m whose view planes are all within 8 degrees of the camera's; a run
seen in neither is not drawn (a chunk with none seen is left out whole). No sample there (the
camera off the path, a cut): nothing is hidden. A .c3b whose run, chunk or draw counts aren't the
level file's is refused at load. Cost: per level draw a pass over the samples (hundreds), per run
of the chunks in view a look at its intervals (usually one or two).

**Why it holds.** The camera of the bake and the game's camera are the same animation; a run drawn
at the game's camera that the bake never saw nearby would have to appear between two samples less
than 0.5 m and 2 degrees apart, beyond the 2-sample and sphere margins. What the bake can't see:
moving level parts (none on the title's path but the ocean, which isn't a level run) and
geometry the bake's settings didn't draw (bake with the defaults).

**To check on a console:** bake, copy the .c3b, `title_vis = on`, watch the flight for missing
pieces (most likely at the start of a loop or after a cut), compare the title's `render ms/frame`
and triangles with it off.

## 4. The depth of a recent frame

The PICA200 has no occlusion queries, so the depth buffer itself is read back.

**The copy** (`ctr_gpu_depth_copy`, both backends). After the first level drawn in near mode in a
frame (the level renderer's end, `draw_level`), the depth drawn so far: the level backgrounds, not
yet the later buckets' models, water and sprites (in Jak 1 a level 0 model bucket comes before
level 1's background). On the 3DS: a partial submit of the frame's commands, then a `GX_TextureCopy`
of the 240 x 400 D24S8 buffer into one linear buffer (375 KB, allocated at the first copy; none:
off), queued after them; it is done once the frame's fence has landed (steady pacing), then
reduced on the CPU to 8x8 blocks, each block's farthest and nearest depth. The buffer's tile
(tx, ty) is the screen's block (ty, 29 - tx) (`write_screenshot`'s mapping), a word's depth its low
24 bits, stored as `0.5 (1 + z / w)` of the level's clip space (`gl_to_pica_mul` with the depth map
-1, 0: the soft backend's z, 0 far, larger nearer). One copy at a time: a frame or two old. Not with
`depth16`, `top_block32` or without the pipeline and steady pacing. The host soft backend takes the
blocks from its depth buffer at once and hands them out from the next frame.

**The test** (`CtrCull.h depth_test`): with the copy's camera, a sphere's screen box (its bounding
box's corners projected) and its nearest point's depth; hidden when that point is farther than the
farthest depth of every block of the box and no block has anything nearer than `depth_reuse_min`
(20 m; Jak, an enemy, a moving platform near the camera may have moved since), spheres grown by
0.5 m. Off screen or reaching behind the camera: not hidden.

**When:** only while the camera is within 0.25 m and 1 degree of the copy's camera (its position
from the matrix, `depth_eye`), and a chunk or run is hidden only from the second copy in a row that
hid it (`depth_reuse_hidden`: per chunk and run the copy and the streak; 3 bytes per run). So it
works while standing or turning slowly, and a thing that comes out from behind something shows at
most a frame or two after the copy that saw it. The vista pass's chunks are not tested.

**What can go wrong:** a moving thing farther than 20 m that hid a run (a far enemy, a platform)
lets the run come back a copy or two late; a blended level draw that writes depth hides what is
behind it.

The test (`cull_test.cpp`): a wall over half the screen at 30 m: a sphere behind it is hidden, one
beside it, in front of it or behind the camera is not, one across its edge partly; a wall at 10 m
hides nothing; the camera's position comes back from its matrix.

**Cost:** 375 KB of linear memory; per copy the GPU copy and a pass over 96k words (1500 tiles);
per chunk in view a projection of 9 points, per run of chunks partly behind the depth the same.

**To check on a console:** `depth_reuse = on`, stand in village1 and the jungle and turn slowly;
look for things missing or blinking behind hills and walls when you start moving, and when Jak or
an enemy walks in front of the scenery.

## 5. Visibility by camera cell and direction

The removed `fine_vis` kept one set per cell for every direction, and cut 0-4% in caves where the
PS2's own visibility already works. This one keeps a set per cell and view direction, from what a
bake actually drew.

**Baking** (host, `CTR_VIS_BAKE=DIR:cells`, the same tagging as section 3). Every frame outside the
title with the camera inside a drawn level's box files the runs seen under the camera's cell (8 m
cubes of the world) and its view direction's bin: 12 yaw bins of 30 degrees x 3 pitch bins (below
-20, level, above +20 degrees). Written to `DIR/<level>.c3c` every 900 recorded frames and when
recording stops: the samples (cell, yaw bin, pitch bin, frames recorded) sorted by cell, the grid
around the recorded cells, per run its samples as intervals, grown by the runs whose spheres touch
(section 3). Play (or run pad scripts) through the places to be covered, looking around in each:
a cell counts only for the directions it was recorded looking at.

**At runtime** (`cell_vis = on`, `cell_vis_select`): the view's direction (its four planes'
normals' sum), a half angle that holds its corners (`atan(sqrt(2) tan(the largest plane angle))`),
and the bins that reaches into (`view_bins`: pitch bins it overlaps; the yaw bins within its half
angle widened for its pitch, + 5 degrees, all twelve once it reaches 60 degrees up or down). The
camera's cell must have every one of those bins recorded in 4 frames or more, else nothing is
hidden. Then the runs seen from that cell and the 7 cells next to it on the camera's side, in those
bins, are the ones drawn: a bit per run (`lev.cell_bits`), made again only when the cell, the side
or the bins change. Runs seen in none are not drawn.

**Why it can still miss something:** the bake saw the cell from the places and angles it was in;
something visible only from another corner of the cell, or at a moment the bake didn't see (a door
opened later, a platform elsewhere), is missing. The 8 cells, the bins' margins and the grown runs
are the safety; the bake's coverage is the rest.

The test (`cull_test.cpp`): for 2000 random views (yaw, pitch, half angle), every direction within
the half angle falls in a bin the view takes; the view's direction and a half angle reaching its
corners from four planes.

**Cost:** on a change of cell, side or bins, a pass over the level's runs (each run's intervals for
the samples taken: up to ~50); per run of the chunks in view a bit. Memory: the .c3c (samples 64
bytes, 4 bytes per run, 4 per interval) and a bit per run.

**To check on a console:** bake by playing a level on the host, copy the .c3c, `cell_vis = on`,
play the same places and look for missing things, most of all in directions the bake looked at
only briefly; compare the triangle counts.

## 6. The PS2's detail distances for a 240-line screen

The PS2 drew 448 lines (Jak 1; Jak II 416), the top screen 240, through the same camera. A shape
difference of `e` at distance `d` covers `e / d x lines / (2 tan(fov / 2))` pixels, so the 3DS shows
at `d x 240 / 448` the error the PS2 showed at `d`. `ps2_lod_scale = auto` multiplies the PS2's
level of detail distances by that ratio (0.54 Jak 1, 0.58 Jak II); a number (0.3-1) sets it.

What it changes (`draw_level`, `ctr_ps2_lod_scale`): the tie groups' PS2 floor (`lod_floor_k`), the
geomorph's mid- and far-planes (`kd`, which `morph_k` hands to the morph rows: the shader's bands
move with the switches), older files' switch distance; the tfrag geomorph's group versions and its
bands (`m_tf_band`, `m_tf_wv`). Not changed: the screen-space error rule (`tie_lod_px`), the
title's 100 m minimum, near chunk reading (still by the PS2's distances: more read than needed),
merc LOD (the game's), shrub and tie distance limits.

Why the picture holds: the switch and the morph move together, so nothing switches unmorphed; the
far versions' own vertex colors (lodnear's "coarse models really close") now show nearer, by the
same factor. That is the thing to look at on a console.

No unit test: it is a factor on existing distances (the host build checks the plumbing).

**To check on a console:** `ps2_lod_scale = auto`; village1, the beach and the title flight: look
at rocks, huts and cliffs as they come nearer (shape and shading changes), compare the triangle
counts and `render ms/frame` with 1.

## 7. Runs smaller than a pixel

A run (up to 8 tie instances, a tfrag node) whose bounding sphere covers under `min_run_px` pixels
of the top screen is not drawn, and neither is a chunk. Shrubs gain most (about half of their
triangles hit no pixel center at the title).

**Size** (`CtrCull.h sphere_px`): with this frame's focal length (pixels of the 400-wide screen per
unit at unit depth, as `tie_lod_px` takes it) and the view's direction, `2 r f d / (z (z - r))`: the
sphere's diameter at its nearest depth, stretched by `d / z` for where it is off the view's axis. It
is never less than the extent of the sphere's projection (the test projects 400 points of each of
3000 random spheres). Reaching to the camera's plane: never hidden.

**Hysteresis:** per chunk and run a flag; one hidden comes back only above 1.25x the limit, so a
run near it doesn't blink as the camera moves. Runs are tested only in chunks far enough for a 10
cm run to be under the limit.

Hidden runs can still be drawn with their neighbours by the small-gap merge, as the view test's.

**Cost:** per chunk in view a dot product, a square root and a division; per run of far chunks the
same. 2 bytes per run of memory while it is on.

**To check on a console:** `min_run_px = 1`; look at far grass, rocks and lamps for things
missing that showed before, compare the triangles (the beach, Misty, the jungle).

## 8. A coarser copy of far plain ties

The vista tier already replaces far scenery beyond the draw distance (500 m) with copies
simplified to metres. Between the PS2's far-planes (30-140 m) and the draw distance a tie is drawn
with its PS2 geometry 3 at full detail. `--far-copies` adds a copy for that range.

**Converter** (`Converter::make_far_copies`, before chunking; off by default because the split
costs draws even with `far_copy` off): tie triangles with one version (no LOD group, wind, morph,
envmap shine, opening planes, hideable prototype, near chunk), grouped by chunking cell, draw and
visibility id. Each group is welded (`vista_vertex_key`: positions, texture coordinates, colors,
time of day kept apart) and simplified by meshoptimizer with an absolute error of
`--far-copy-error` (0.25 m; topology kept, no sloppy step); a group whose copy is at least 20%
smaller becomes sources (tier 5) and its copy is added (tier 6), both in the group's cell. The
copy's corners are source vertices (the same time of day colors and texture coordinates); its draw
has no back face culling mark (a simplified closed surface may open). Both are written as ordinary
tie chunks (`lod_tier` 0, so every tie rule applies to them) with a role in `ExtFarCopy`; sharing a
cell, they share `lod_center`. The vista tier copies the sources (tier 5), not the copies. The log
has `<level>: far copies: N groups, N tie triangles copied as N (N%)`.

**Runtime** (`emit_chunk`): a source chunk is not drawn, and its cell's copy chunk is, while the
camera is `far_copy_distance` (150 m) or more from the cell's center; nearer, the other way round.
One shared center for both: never both, never neither, at the cost of a switch of the whole cell at
once. With `far_copy` off the copies are never drawn.

At 150 m, 0.25 m is about 0.6 pixels of the top screen (`f ~ 400 px` per unit of size at unit
depth): the switch should not show, except where a copy's colors differ (it keeps source vertices,
so they don't).

The test (`converter_test.cpp test_far_copies`): a finely cut flat tie gets a copy of a few
triangles in its cell, from its own vertices, without the culling mark; a wind instance gets none.

**To check on a console:** convert with `--far-copies`, `far_copy = on`; watch tie-heavy views
(village1 from the beach, the title flight) for a cell's ties changing shape at 150 m; compare
triangles and draws with it off (the split itself adds draws).
