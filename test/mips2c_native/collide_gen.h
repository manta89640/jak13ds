#pragma once

/*!
 * @file collide_gen.h
 * (AI-assisted)
 * Generators for the collision structures (collide-cache, collide-frag-mesh, the scratchpad
 * vertices) shared by the collide tests.
 */

#include "fakes.h"

namespace tests {

// collide-cache (basic): num-tris 0, num-prims 4, ignore-mask 8, proc 12, collide-box 28,
// collide-box4w 60, collide-with 92, prims 108 (100 x 48 bytes), tris 4908 (64 bytes each)
constexpr u32 kCachePrims = 108;
constexpr u32 kCacheTris = 4908;

//! the collide-cache type, with the mips2c methods bound (once)
u32 collide_cache_type();

//! set *collide-cache-max-tris* (the mips2c code reads it through a host pointer)
void set_max_tris(u32 n);

//! a collide-cache with room for max_tris triangles, header fields random
u32 gen_cache(Gen& g, u32 max_tris);

//! a random collide-cache-tri at tri (vertices within size of center)
void gen_cache_tri(Gen& g, u32 tri, const float center[4], float size);

/*!
 * A collide-frag-mesh with vertex_count vertices: packed vertex data (u16 x, y, z), triangle
 * strips over those vertices, pat indices and a pat array. Returns the mesh (a basic).
 */
u32 gen_frag_mesh(Gen& g, int vertex_count);

//! fill the scratchpad vertices (32 bytes each: ints, then floats) like method 32 does
void gen_spad_vertices(Gen& g, int count, s32 range);

//! a random collide-work (sphere, box, inverse matrix), set as *collide-work*
u32 gen_collide_work(Gen& g, s32 box_range);

}  // namespace tests
