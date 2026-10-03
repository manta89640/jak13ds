/*
 * (AI-assisted)
 * std::chrono::steady_clock::now() without 64-bit divisions.
 *
 * libstdc++'s version calls clock_gettime(CLOCK_MONOTONIC), which libctru computes from the ARM11
 * system tick with two __aeabi_uldivmod calls (tick / SYSCLOCK_ARM11 and the remainder in ns).
 * The ARM11 has no divide instruction, so each is a libgcc routine of a few hundred cycles, and
 * now() runs in the IOP kernel's scheduling loop, the sound handler and the frame timers. This
 * converts the tick with a 32.32 fixed-point multiply instead: same epoch (boot), monotonic, and
 * within 1 ns per 16 s of uptime of the libctru value.
 *
 * Linked into gk as an object, so these definitions replace libstdc++'s chrono.o; it also
 * defines system_clock::now(), so that one is repeated here unchanged.
 */

#include <chrono>
#include <ctime>

#include <3ds.h>

namespace {
// 2^32 * 1e9 / SYSCLOCK_ARM11 (268111856 Hz), rounded: 3 + 0xbad34aef / 2^32 ns per tick
constexpr u32 kNsPerTickInt = 3;
constexpr u32 kNsPerTickFrac = 0xbad34aefu;
static_assert(SYSCLOCK_ARM11 == 268111856, "update the tick to ns factor");
}  // namespace

std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept {
  const u64 t = svcGetSystemTick();
  const u32 lo = (u32)t, hi = (u32)(t >> 32);
  const u64 ns = (u64)hi * (((u64)kNsPerTickInt << 32) | kNsPerTickFrac) +
                 (u64)lo * kNsPerTickInt + (((u64)lo * kNsPerTickFrac) >> 32);
  return time_point(duration((rep)ns));
}

std::chrono::system_clock::time_point std::chrono::system_clock::now() noexcept {
  timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  return time_point(duration(std::chrono::seconds(tp.tv_sec) + std::chrono::nanoseconds(tp.tv_nsec)));
}
