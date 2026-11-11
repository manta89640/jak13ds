// (AI-assisted, cpubench) CtrCpuBench's cases as ARMv6K code under qemu-arm (run.sh): every A/B
// pair's checksums must match, with the FPSCR the 3DS gives its threads (flush to zero, default
// NaN; round toward zero per the emulators' sources, run.sh's second run). Times are meaningless
// under qemu.
#include <cstdio>
#include <cstdlib>

#include "game/graphics/ctr/CtrCpuBench.h"

int main(int argc, char** argv) {
  const unsigned fpscr = argc > 1 ? (unsigned)strtoul(argv[1], nullptr, 16) : 0x03000000u;
  __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
  const int bad = ctr_cpu_bench::run(
      0.001, [](const char* t, void*) { printf("%s\n", t); }, nullptr);
  printf("fpscr %08x: %d mismatches\n", fpscr, bad);
  return bad ? 1 : 0;
}
