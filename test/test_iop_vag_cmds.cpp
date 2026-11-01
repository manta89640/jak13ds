#include <chrono>
#include <cstdlib>
#include <future>
#include <set>
#include <thread>

#include "game/overlord/jak1/iso_queue.h"
#include "game/sce/iop.h"
#include "game/system/iop_thread.h"
#include "gtest/gtest.h"

// More than 32 Jak 1 VAG commands queued at once (the ISO thread in one slow card read while the
// PLAY and player RPCs queue a command per frame): with a 32-bit mask GetVAGCommand spun forever
// on the 33rd, inside the IOP's cooperative scheduler (the hardware title freeze).
TEST(IopVagCommands, MoreThan32Queued) {
  static IOP iop;
  iop::LIBRARY_register(&iop);
  jak1::iso_queue_init_globals();
  jak1::InitBuffers();

  constexpr int kQueued = 63;  // GetVAGCommand waits for a free one from 63 on
  auto task = std::async(std::launch::async, [] {
    std::set<jak1::VagCommand*> got;
    for (int i = 0; i < kQueued; i++) {
      got.insert(jak1::GetVAGCommand());
    }
    return got;
  });
  if (task.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    ADD_FAILURE() << "GetVAGCommand did not return with fewer than 63 commands in use";
    fflush(stdout);
    std::_Exit(1);
  }
  auto got = task.get();
  EXPECT_EQ(got.size(), (size_t)kQueued);

  for (auto* cmd : got) {
    jak1::FreeVAGCommand(cmd);
  }
  std::set<jak1::VagCommand*> again;
  for (int i = 0; i < kQueued; i++) {
    again.insert(jak1::GetVAGCommand());
  }
  EXPECT_EQ(again, got);
}
