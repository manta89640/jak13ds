#pragma once

/*!
 * @file CtrCpuBench.h
 * (AI-assisted, cpubench) CPU microbenchmarks for the 3DS's ARM11 MPCore and its VFP11 (config.ini
 * cpu_bench = on; docs/3ds-port/performance.md "CPU instruction costs on the console").
 *
 * Azahar counts about one tick per guest instruction: it doesn't model the VFP's 8-cycle result
 * latency, the pipeline drain of a vmrs, cache misses or branch mispredicts. So instruction
 * level changes that remove stalls (not instructions) can only be priced on a console. Each case
 * runs a kernel for a number of iterations and reports the time per iteration; the cases of an
 * A/B pair compute the same checksum (the bench reports a MISMATCH otherwise), so a console run
 * also checks that a new kernel gives the old kernel's results there.
 *
 * No 3DS dependencies but the timer (svcGetSystemTick on the 3DS, steady_clock elsewhere): the
 * host's regression test runs every case with a few iterations for the checksums, and
 * test/ctr_render/arm/run.sh runs them as ARMv6K code under qemu-arm.
 */

#include <cstdint>

namespace ctr_cpu_bench {

struct Case {
  const char* name;
  //! the kernel: n iterations, a checksum of what it computed (kept so nothing is optimized away)
  uint32_t (*fn)(uint32_t n);
  uint32_t iters;     //! iterations on the console (each case runs a few ms there)
  const char* pair;   //! the case whose checksum this one must match, or nullptr
};

//! Every case, in order; *count is set to their number.
const Case* cases(int* count);

//! Runs every case (scale: the iterations' factor, e.g. 1/1000 for a quick identity check),
//! writing one line per case through line(); returns the number of checksum mismatches.
int run(double scale, void (*line)(const char* text, void* ctx), void* ctx);

}  // namespace ctr_cpu_bench
