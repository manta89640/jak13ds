// (AI-assisted) tod_diff on ARM (tod_arm.py): the level file, the trace and the job at fixed
// addresses, then sim_main (tod_diff.cpp) on one version with its phases marked.
#include <cstdint>

extern "C" int sim_main(const uint8_t*, const uint32_t*, int, int, int, int, uint32_t*);
struct Job {
  uint32_t frames, mode, stride, step;
  uint32_t out[8];
};
#define JOB ((Job*)0x0f000000)
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
extern "C" {
__attribute__((noinline, used)) void tod_arm_done() {
  for (;;) {
    asm volatile("" ::: "memory");
  }
}
__attribute__((used)) void tod_arm_start() {
  for (auto* f = __init_array_start; f != __init_array_end; f++) {
    (*f)();
  }
  sim_main((const uint8_t*)0x20000000, (const uint32_t*)0x0c000000, JOB->frames, JOB->mode,
           JOB->stride, JOB->step, JOB->out);
}
__attribute__((naked, used, section(".text.start"))) void _start() {
  asm volatile(
      "ldr sp, =0x0e100000\n"
      "bl tod_arm_start\n"
      "b tod_arm_done\n");
}
}
