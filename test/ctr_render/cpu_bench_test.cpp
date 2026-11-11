// (AI-assisted, cpubench) CtrCpuBench's A/B pairs give the same checksums on the host (the ARM
// paths: test/ctr_render/arm/run.sh under qemu-arm).
#include <stdexcept>

#include "game/graphics/ctr/CtrCpuBench.h"

#include "fmt/format.h"

void run_cpu_bench_tests() {
  const int bad = ctr_cpu_bench::run(0.001, [](const char*, void*) {}, nullptr);
  if (bad) {
    throw std::runtime_error(fmt::format("cpu_bench: {} A/B pairs differ", bad));
  }
  int n = 0;
  ctr_cpu_bench::cases(&n);
  fmt::print("  cpu_bench: {} cases, every A/B pair the same\n", n);
}
