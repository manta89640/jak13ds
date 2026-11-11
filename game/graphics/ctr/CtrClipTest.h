#pragma once

/*!
 * @file CtrClipTest.h
 * Boot self-test of the GPU's clipping of big triangles next to the camera: a ground quad
 * reaching behind the camera (as level ground, walls and the ocean do when the camera is right
 * next to them), cut at the near plane on the CPU, with w = 1 (the ocean's), with back faces
 * culled, and with the PICA's user clip plane at the near plane. Each case's pixels are compared
 * with exact clipping (the host); one log line. Where the GPU's own clipping loses pixels and the
 * clip plane gives them back, the plane stays on (ctr_gpu_set_near_clip_default). The culled cases
 * say whether the GPU decides facing before it clips (CtrLevel draws culled draws that cross the
 * camera's plane both sides either way).
 */

void ctr_clip_selftest();
