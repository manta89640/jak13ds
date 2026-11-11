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
| 4 | `depth_reuse` | runs and chunks a recent frame's depth hid, while the camera holds still | nothing |
| 6 | `ps2_lod_scale` | detail the 240-line screen can't show (coarse versions nearer) | nothing |
| 7 | `min_run_px` | runs and chunks under a pixel or so across | nothing |
| 8 | `far_copy` | detail of far plain ties (a simplified copy from 150 m) | levels converted with `--far-copies` |
| 9 | `cull_terrain` | back faces of ground the camera can't get under | levels converted with `--terrain-cull` |
| 10 | `cone_cull` | the vertex work of runs whose every triangle faces away | levels converted with `--face-runs` |

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
(20 m; Jak, an enemy, a moving platform near the camera may have moved since). Spheres are grown by
0.5 m plus the parallax of the camera's move since the copy (`depth_grow`: the move times the
sphere's distance over `depth_reuse_min`, the nearest an occluder can be). Any part off the copy's
screen (within a pixel of its edge) or reaching behind the camera: not hidden (the copy has no depth
there, and a turn since may have brought it in view).

**When:** a copy is asked for only while the camera is slow (within 0.125 m and about 0.5 degree of
the frame before), and used only while the camera is within 0.25 m and 1 degree of the copy's
camera (its position from the matrix, `depth_eye`), and a chunk or run is hidden only from the
second copy in a row that hid it (`depth_reuse_hidden`: per chunk and run the copy and the streak;
3 bytes per run). So it works while standing or turning slowly, and a thing that comes out from behind something shows at
most a frame or two after the copy that saw it. The vista pass's chunks are not tested.

**What can go wrong:** a moving thing farther than 20 m that hid a run (a far enemy, a platform)
lets the run come back a copy or two late; a blended level draw that writes depth hides what is
behind it.

The test (`cull_test.cpp`): a wall over half the screen at 30 m: a sphere behind it is hidden, one
beside it, in front of it or behind the camera is not, one across its edge partly; a wall at 10 m
hides nothing; a sphere behind a wall over the whole screen that reaches off its edge is not hidden;
the growth for a move; the camera's position comes back from its matrix.

**Cost:** 375 KB of linear memory; per copy the GPU copy and a pass over 96k words (1500 tiles);
per chunk in view a projection of 9 points, per run of chunks partly behind the depth the same.

**To check on a console:** `depth_reuse = on`, stand in village1 and the jungle and turn slowly;
look for things missing or blinking behind hills and walls when you start moving, and when Jak or
an enemy walks in front of the scenery.

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

## 9. Back faces of the ground

Tfrag is drawn double-sided (the PS2 draws it so); at the title ~43% of the level triangles were
back faces of draws without culling. The GPU's back face culling saves their setup and pixels, not
their vertices.

**Which ground** (`Converter::mark_terrain_cull`, `--terrain-cull`; off by default because the
marked triangles' draws split from the others): opaque tfrag (`key_cullable`: no blending that
stays, an alpha test that never fails) of tiers 0-2, floor-like (|normal y| >= 0.7), that is
- walkable ground itself: the level's collision has ground within 1 m of the triangle's center
  height there (not an overhang, a roof or a bridge over a gap), and
- not near lower ground: no collision column (4 m) within 20 m has ground more than 2 m below the
  triangle's plane there (a ledge, a pit's rim, the top of a cliff are left alone: from below them
  the camera could look up at the back).

The camera stays above the ground near it, so it stays above such a plane and never sees the back;
seen from further away, the ground's own mass (the cliffs, the slopes) is in front of it. The
triangle is turned to face up (the culled side is the underside) and its draw gets
`kDrawCullTerrain`. Levels without collision in their .fr3: none. The vista tier's copies drop the
mark (they are seen from far away, maybe from below). `cull_fixup` takes it back from draws whose
blending stays, as the tie marks.

**Runtime:** `cull_terrain = on` gives those draws `CTR_TCC_CULL_BACK` at level load, culled with
`cull_ties`' winding (so `cull_ties` must be on). Off: both sides, as before.

The test (`converter_test.cpp test_terrain_cull`): ground at 0 m with a 10 m deep pit: a floor
triangle far from the pit is marked and turned to face up; one at the pit's edge, one floating
10 m over walkable ground and a wall are not.

**What can go wrong:** the camera dropping under the ground (a cutscene camera, a fall through the
floor) sees through the marked ground; ground drawn as tfrag but walked on as something else
(collision of a moving platform) is left alone by the collision test.

**To check on a console:** convert with `--terrain-cull`, `cull_terrain = on`; walk the beach, the
jungle and village1 looking for holes in the ground (most likely by ledges the test missed and in
cutscenes); compare triangles and `render ms/frame`.

## 10. Runs that face away, skipped on the CPU

The GPU culls back faces of the marked draws (closed tie surfaces, and the ground of section 9)
after their vertex program has run. A run all of whose triangles face away can be left out before:
the same picture, without its vertex work. A tie node's run holds a whole rock or house (normals
every way), so the converter first splits such runs by the way their triangles face.

**Converter** (`--face-runs`, off by default: more runs, a slightly worse vertex cache):
- each triangle of a back face culled draw (`kDrawCullBack` or `kDrawCullTerrain`; not
  `kDrawCullCond`, culled only from some places; not moved by the wind) gets a facing bin
  (`face_bin`: the largest component of its normal and its sign, 6 bins); a draw's triangles of one
  node are sorted by it and runs split where it changes;
- per run its normal cone (`CtrCull.h make_cone`), from the written (welded, quantized) vertices
  the run's indices point at: the mean normal as axis, packed to s8, and the widest normal from
  that packed axis, 3 degrees wider (6 if the chunk's vertices have morph offsets), its cosine
  rounded down; 80 degrees or wider: none. In `ExtCones`.

**Why it holds** (`cone_away`): with the run's sphere (center c, radius r), the camera at e and
v = c - e at angle t from the axis, every normal is within t + a of v's direction, so for every
point p of the run n . (p - e) >= n . v - r >= |v| cos(t + a) - r; the run is left out when that is
positive (every triangle has the camera behind its plane). The test (`cull_test.cpp test_cones`):
random patches of triangles and cameras: every run the cone leaves out has each triangle facing
away (24000 views checked, ~23% left out).

**Runtime** (`cone_cull = on`, needs `cull_ties`): in the run pass, a run with a cone whose draw
is culled (`CTR_TCC_CULL_BACK` in its state: so terrain only with `cull_terrain`) and that faces
away is not drawn. Per run of the chunks in view: two square roots and a few products.

**To check on a console:** convert with `--face-runs`, `cone_cull = on` (and `cull_ties`);
nothing should look different; compare `render ms/frame` (the GPU's vertex work) and the runs
and triangles hidden in the log.
