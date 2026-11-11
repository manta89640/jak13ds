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
