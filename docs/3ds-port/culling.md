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
| 6 | `ps2_lod_scale` | detail the 240-line screen can't show (coarse versions nearer) | nothing |
| 8 | `far_copy` | detail of far plain ties (a simplified copy from 150 m) | levels converted with `--far-copies` |
| 9 | `cull_terrain` | back faces of ground the camera can't get under | levels converted with `--terrain-cull` |

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
