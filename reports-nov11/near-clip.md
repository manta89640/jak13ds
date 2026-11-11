# near-clip: surfaces right next to the camera vanishing on hardware

Branch `worktree-agent-a4e8b74487d8fbe68`, rebased on nov10 dcbee78c40. Commit 40b90488f5.
Host checks only (small-memory C-mode `gk --ctr-gfx`, `CTR_SOFT_CULL_PROBE=1`). No 3DS build or
emulator run.

## Verdict

**Not settled on hardware.** No log shows near geometry vanishing, and Azahar can't show any
GPU-side cause. What I found:

- **Leading mechanism (the lead's hypothesis 1b):** GPU back-face culling decided before
  clipping. I quantified it on the host. It explains walls, rocks and buildings in both games
  (cull_ties) and the Haven City ground (cull_terrain). It does not explain the Jak 1 ground,
  which is double-sided tfrag in the c3l-nov01 files, or the ocean, which has no culling.
- **Fix:** structural and cheap. It is in the commit and runs whether or not the hardware has
  the problem.
- **Boot self-test:** measures the hardware's clipping and culling of near triangles in one log
  line. The next batched hardware run settles the rest.

## What changes frame to frame for a big surface next to the camera

Its triangles start crossing the camera's plane, so clip w has both signs within one triangle.
Every other stage sees the same sphere and the same chunk as one frame earlier. Ranked:

1. **GPU facing decided on the unclipped corners (cull before clip).**
   - A corner with w < 0 projects mirrored, so the screen winding of the unclipped triangle flips
     when exactly one or three of its w are negative.
   - Such a front face is culled whole, while the visible part in front of the near plane is
     often large.
   - Only draws with `CTR_TCC_CULL_BACK` are affected: cull_ties and cull_terrain (ties and the
     Jak II ground), plus cull_merc.
   - **Host model** (soft renderer, near-crossing triangles of culled draws, true facing from the
     clipped polygon against the unclipped triangle's projected winding):
     - Haven City slums walk (`jak2_city`-like pad into walls, camera turns): 4-15 near-crossing
       culled front faces per frame, covering 1.5k-15k px. Of those, **0.7k-6.6k px per frame
       would be lost**, up to 7% of the screen, flickering with the camera.
     - A homogeneous-determinant cull would lose none.
     - Jak 1 village walk: ~0 such triangles. Jak 1's ground isn't culled, and the camera rarely
       cuts into ties.
2. **GPU guard band / huge screen coordinates.**
   - Near-crossing triangles clipped at the near plane have corners at 30-1000 NDC; dozens per
     frame in the Jak 1 walk.
   - The visible ones reach 4-30 NDC and cover up to ~20k px per frame.
   - Against this: the hardware self-test from Nov 9 drew an uncut prism 10 NDC (1080 px) off
     screen correctly (hw-nov11b jak1 gk.log:1497). The sky's roof triangles cross w = 0 every
     frame, and the sky is drawn right on hardware.
   - Not ruled out for 30-1000 NDC.
3. **No near-plane clip in the PICA** (only z <= 0 and w > 0).
   - The sky crosses w = 0 through the z <= 0 plane and works; level geometry crosses it
     through the near plane (z = -w).
   - The self-test's "behind" cases measure this. The PICA's user clip plane (regs 0x47-0x4B,
     never written by citro3d) at z + w >= 0 would fix it, and the self-test turns that plane on
     by itself if it helps.
4. **f24 vertex math: ruled out.**
   - The near-plane test is well conditioned: z_pica + w is about w (1 - d_near / depth), not a
     small difference.
   - Position error for a vertex 0.3 m away in a 50 m chunk is about 0.4%.
5. **CPU culling: not seen.**
   - Host walks in Sandover (hut, ledge, bridge) and the Haven slums, with the camera against
     walls and dumped every 3-5 frames, showed no vanishing.
   - The pop / hole counters in tonight's logs show no near holes. Holes are tie chunks only,
     and pops are LOD switches.
   - Lone register writes are not the cause: the stencil self-test passes on hardware.
   - Cross-core cache flushes are not the cause: the flush self-test is 0/0 on hardware.
6. **Ocean.**
   - The ocean has no face culling; CtrOceanBatch clips depth on the CPU and sends w = 1.
   - Tonight's logs have no dropped draws or "no room" lines for the ocean or the levels. The
     ocean-near bucket skipped on a full vertex buffer (earlier Jak II sessions, 14-149 dropped
     draws per window) is the best explanation for "at some point".
   - The self-test's "cut at near w=1" case checks the ocean's huge-NDC path.

## Fix (40b90488f5)

### CtrLevel.cpp, per level draw, after the kDrawCullCond pass

- Culled draws (`CTR_TCC_CULL_BACK`) with a run whose sphere crosses the camera's plane are drawn
  both sides that frame. The test is |w(center)| < (r + 1 m) x |dw|, where w is the level clip
  matrix's w row.
- Chunk sphere first, then the draw's run spheres.
- Their state is restored at the next level draw.
- Log: `[ctr] <slot>: culled draws drawn both sides across the camera's plane: N draws, T
  triangles per level draw`.
- This undoes cull_ties / cull_terrain / cone-style culling only where it is unsafe.
- **What that culling saves there:** setup and fill of ~half the triangles of 2-5 draws next to
  the camera.

### Cost (host, Haven slums walk)

- 1.9-4.9 draws and 514-1292 triangles per level draw lose their culling. About half of those
  triangles are back faces that now reach setup; most fail the depth test.
- At the measured 0.11-0.14 us per triangle that is **~0.03-0.09 ms GPU per frame**.
- CPU: one loop over the visible chunks, which is negligible.
- Jak 1 village: about 0.

### Result (host)

- Near-crossing culled front faces: 1.5k-15k px per frame before, 0 after (same walk).
- Picture otherwise unchanged.

### Boot self-test (CtrClipTest.cpp, render frame 4, after the shadow self-test)

- **Cases:** 10 frames. A ground quad 0.5 m below the camera:
  - in front;
  - reaching 2, 20 and 200 m behind;
  - cut at the near plane on the CPU;
  - the same with w = 1;
  - in front, culled;
  - reaching behind, culled;
  - reaching behind with the user clip plane, at 20 m and 200 m.
- **Method:** each case is drawn over a background at 50 m with GEQUAL, and its pixels are read
  back. Expected values come from the host's exact clipping: 37900, 37900, 46000, 47200, 46000,
  46000, 37900, 46000, 46000, 47200. On the host every case matches.
- **Log line:**
  `[ctr] near clip self-test (pixels, expected): ... -> near clip plane on|off; facing right|
  decided before clipping|unknown (culled in front)`.
- **Decision:** if the "behind" cases lose pixels and the clip-plane cases don't, the user clip
  plane goes on for every draw. It is 5 command words per frame, written in frame_state_reset.
- **Boot cost:** ~10 extra GPU frames with a top-buffer readback at frame 4 (estimated
  ~0.1-0.2 s, not measured).

## Settling it on hardware (the lead's next batched run, no extra steps)

Read the `near clip self-test` line in gk.log.

| Result | Meaning |
|---|---|
| "facing decided before clipping" | Cause 1 confirmed; the fix is already in. |
| "behind" cases WRONG, clip plane cases right | Cause 3; the plane is on by itself. |
| "cut at near" / "w=1" WRONG | Guard-band limit (cause 2, the ocean too); needs CPU clipping of near triangles. Not done. |
| All right | The GPU clips and culls correctly, and the vanishing is something else. The fix only costs ~0.05 ms. |

Also check the new `drawn both sides` lines.

## Not settled / left

- What the PICA really does: the self-test answers it.
- Jak 1 tfrag ground vanishing has no explanation from culling. If the self-test is all right,
  it needs a hardware shot of the exact spot.
- cull_merc: merc models crossing the camera plane are not unculled. This is rare, since the
  camera is seldom inside a character.
- Jak II tie LOD switches: 100-855 in-view pops per 15 s on hardware tonight. That is a separate
  "flicker" source, outside this bug.
