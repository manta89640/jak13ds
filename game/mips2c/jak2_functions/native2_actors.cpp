/*!
 * @file native2_actors.cpp
 * Jak 2 actors-update (entity.gc): the *vis-actors* loop over a level's entities runs every frame
 * and does nothing for almost every entity (the visible ones are alive, the others dead).
 * pc-actors-vis-next finds the next entity the GOAL loop body would act on (birth! or kill!), with
 * the loop's tests done the same way (pure loads and compares): is-object-visible?, the perm and
 * kill-mask bits, and the PC port's valid? checks of the entity type. Whatever it isn't sure of
 * (another is-object-visible? method, the distance test, the name blacklist, a valid? that might
 * fail and print) goes to the GOAL code, which then runs the original body for that entity.
 *
 * (pc-actors-vis-next level links index count cam-pos task-mask stop-births? births) -> index
 * OPENGOAL_ACTORS_VERIFY=1 (host): every skipped entity is checked with pc-actors-vis-skip? (the
 * GOAL tests of the loop, entity.gc).
 */

#include <cstdlib>

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native2.h"

extern u32 g_link_generation;  // game/kernel/common/klink.h

namespace Mips2C::jak2::native2 {

namespace {

// level
constexpr u32 kLevelEntity = 244;
constexpr u32 kLevelVisBits = 2388;
constexpr u32 kLevelAllVisible = 2392;
constexpr u32 kLevelVisInfo0 = 2404;
// entity-links (64 bytes, from +12 of the entity-links-array)
constexpr u32 kLinkEntity = 8;
constexpr u32 kLinkProcess = 12;
constexpr u32 kLinkVisId = 20;
constexpr u32 kLinkKillMask = 24;
constexpr u32 kLinkPermStatus = 56;
// entity-actor etype, process mask, *ACTOR-bank* birth-max, *pc-settings* ps2-actor-vis?
constexpr u32 kEntityActorEtype = 52;
constexpr u32 kProcessMask = 4;
constexpr u32 kActorBankBirthMax = 8;
constexpr u32 kPcSettingsPs2ActorVis = 316;
constexpr u32 kEndOfSmallMemory = 0x3000000;

struct Syms {
  u32 gen = ~0u;
  u32 pc_settings, actor_bank, level_type, entity_actor, object, type, symbol;
  u32 visible_method = 0;  // is-object-visible? (level method 10) at the first call
} g_syms;

void resolve(Syms& s) {
  if (s.gen == g_link_generation) {
    return;
  }
  s.gen = g_link_generation;
  s.pc_settings = sym_value_addr("*pc-settings*");
  s.actor_bank = sym_value_addr("*ACTOR-bank*");
  s.level_type = gload<u32>(sym_value_addr("level"));
  s.entity_actor = gload<u32>(sym_value_addr("entity-actor"));
  s.object = gload<u32>(sym_value_addr("object"));
  s.type = gload<u32>(sym_value_addr("type"));
  s.symbol = gload<u32>(sym_value_addr("symbol"));
  if (!s.visible_method) {
    s.visible_method = gload<u32>(s.level_type + 16 + 4 * 10);
  }
}

//! type-type? (gcommon.gc)
bool type_type(u32 t, u32 parent, const Syms& s) {
  if (parent == s.object) {
    return true;
  }
  while (t != s.object && t != 0) {
    if (t == parent) {
      return true;
    }
    t = gload<u32>(t + 4);
  }
  return false;
}

//! valid? obj type / symbol with a #f allow-false: true only when it is surely valid (then it
//! prints nothing); false: not sure, the GOAL code decides
bool surely_valid_type(u32 obj, const Syms& s) {
  return (obj & 7) == 4 && obj < kEndOfSmallMemory && gload<u32>(obj - 4) == s.type;
}
bool surely_valid_symbol(u32 obj, u32 st) {
  return (obj & 1) != 0 && obj >= st - 32768 && obj < st + 32768 && obj < kEndOfSmallMemory;
}

enum class Test { kTrue, kFalse, kGoal };

//! the and of the loop's cond for one entity
Test vis_test(u32 level, u32 link, u32 task_mask, u32 st, const Syms& s) {
  const u32 f = st;
  const u32 pcs = gload<u32>(s.pc_settings);
  if (pcs != f && gload<u32>(pcs + kPcSettingsPs2ActorVis) == f) {
    return Test::kGoal;  // the name blacklist
  }
  if (gload<u32>(s.level_type + 16 + 4 * 10) != s.visible_method ||
      gload<u32>(level - 4) != s.level_type) {
    return Test::kGoal;
  }
  // is-object-visible?: the vis bit (sign of the byte shifted left by 56 + bit)
  const s32 id = gload<s32>(link + kLinkVisId);
  const s8 byte = gload<s8>(gload<u32>(level + kLevelVisBits) + (u32)(id >> 3));
  if (!((u8)byte & (0x80u >> (id & 7)))) {
    return Test::kFalse;
  }
  if (gload<u16>(link + kLinkPermStatus) & 0x600) {
    return Test::kFalse;
  }
  if (gload<u32>(link + kLinkKillMask) & task_mask) {
    return Test::kFalse;
  }
  if (gload<u32>(level + kLevelVisInfo0) == f) {
    return Test::kGoal;  // the distance test
  }
  const u32 e = gload<u32>(link + kLinkEntity);
  if (!type_type(gload<u32>(e - 4), s.entity_actor, s)) {
    return Test::kTrue;
  }
  const u32 etype = gload<u32>(e + kEntityActorEtype);
  if (!surely_valid_type(etype, s)) {
    return Test::kGoal;
  }
  const u32 sym = gload<u32>(etype);
  if (!surely_valid_symbol(sym, st)) {
    return Test::kGoal;
  }
  const u32 value = gload<u32>(sym - 1);
  if (!surely_valid_type(value, s)) {
    return Test::kGoal;
  }
  return etype == value ? Test::kTrue : Test::kFalse;
}

//! would the loop body do nothing for this entity? false: not sure (the GOAL body runs)
bool skippable(u32 level, u32 link, u32 task_mask, u32 stop_births, u32 st, const Syms& s) {
  const u32 f = st;
  const u32 proc = gload<u32>(link + kLinkProcess);
  switch (vis_test(level, link, task_mask, st, s)) {
    case Test::kTrue:
      // birth! unless it has a process, is dead, or births stopped
      return proc != f || (gload<u16>(link + kLinkPermStatus) & 5) || stop_births != f;
    case Test::kFalse:
      // kill! if it has a process without no-kill
      return proc == f || (gload<u16>(link + kLinkPermStatus) & 8) ||
             (gload<u32>(proc + kProcessMask) & 0x1000);
    default:
      return false;
  }
}

#ifndef __3DS__
bool verify_on() {
  static const bool on = [] {
    const char* v = getenv("OPENGOAL_ACTORS_VERIFY");
    return v && v[0] == '1';
  }();
  return on;
}
#endif

u64 pc_actors_vis_next_impl(const NativeArgs& args) {
  const u32 level = (u32)args.a[0];
  const u32 links = (u32)args.a[1];
  s64 i = (s64)args.a[2];
  const s64 count = (s64)args.a[3];
  const u32 task_mask = (u32)args.a[5];
  const u32 stop_births = (u32)args.a[6];
  const s64 births = (s64)args.a[7];
  const u32 st = (u32)args.st;
  static int on = -1;
  if (on < 0) {
    on = native_turned_off("pc-actors-vis-next") ? 0 : 1;
  }
  resolve(g_syms);
  // the loop returns after the first entity once births reach birth-max
  if (!on || births >= (s64)gload<s32>(gload<u32>(g_syms.actor_bank) + kActorBankBirthMax)) {
    return (u64)i;
  }
  for (; i < count; i++) {
    const u32 link = links + 12 + 64 * (u32)i;
    if (!skippable(level, link, task_mask, stop_births, st, g_syms)) {
      break;
    }
#ifndef __3DS__
    if (verify_on()) {
      static const u32 goal_fn = sym_value_addr("pc-actors-vis-skip?");
      static u64 checks = 0, mismatches = 0;
      const u64 a[8] = {level, link, args.a[4], task_mask, stop_births};
      const bool goal = (u32)native_call_goal(gload<u32>(goal_fn), a, args) != st;
      checks++;
      if (!goal) {
        mismatches++;
        printf("actors verify MISMATCH: entity #x%x of level #x%x skipped, GOAL acts on it\n",
               gload<u32>(link + kLinkEntity), level);
      }
      if ((checks & 0xffff) == 0) {
        printf("actors verify: %llu skipped entities checked, %llu mismatches\n",
               (unsigned long long)checks, (unsigned long long)mismatches);
      }
    }
#endif
  }
  return (u64)i;
}

}  // namespace

const NativeImpl pc_actors_vis_next =
    MIPS2C_NATIVE_IMPL(pc_actors_vis_next_impl, NATIVE_CALLS_GOAL, 0);

}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::pc_actors_natives {
void link() {
  gLinkedFunctionTable.reg("pc-actors-vis-next", native2::pc_actors_vis_next.as_exec, 64,
                           &native2::pc_actors_vis_next);
}
}  // namespace Mips2C::jak2::pc_actors_natives
