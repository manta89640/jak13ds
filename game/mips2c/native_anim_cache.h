#pragma once

/*!
 * @file native_anim_cache.h
 * Per-actor cache of the decompressed joint animation frame (Jak 1 and Jak 2 native
 * new-joint-decompressor, native_anim.cpp).
 *
 * The output frame is a pure function of the decompression requests (animation, frame, frame
 * interpolation, weight) and the number of joints, as long as the animation data at those
 * addresses doesn't change. A fixed-pose actor (platforms, arms, discs, buttons, barrels) makes the
 * same requests every frame. The slot of a joint-control remembers its last requests; when the
 * next call makes the same ones, its result is stored, and the calls after that copy it.
 *
 * Exactness:
 * - the stored part ("head") is what init-frame! zeroes: the 2 matrices and num-joints - 2
 *   transformqs, after finalize-frame!. The decompression loops also add to the 2 transformqs
 *   after it ("tail"), which hold whatever was there before; a hit replays those 2 joints of
 *   every request from the saved stream positions, so the tail gets the same bits.
 * - the blend tree, the channels' inspector-amount and the requests are computed on every call
 *   (they are cheap and fill the decompressor's work area as the GOAL code does).
 * - stale data: a slot is only valid for the g_link_generation it was made in (every object file
 *   link, so every level or art group load), and never used for a spooling joint-control (streamed
 *   animation frames are loaded into reused buffers).
 */

#include <cstdlib>
#include <cstring>

#include "common/common_types.h"

extern u32 g_link_generation;  // game/kernel/common/klink.h

namespace Mips2C::anim_cache {

constexpr int kSlots = 256;
constexpr int kMaxReqs = 4;
constexpr u32 kTailBytes = 64;       // per request: the game's stream positions at the tail
constexpr u32 kMaxHead = 4096;       // bytes of head per slot (up to 82 joints)
constexpr u32 kBudget = 128 * 1024;  // bytes stored in all slots

//! one decompression request as build-requests! writes it (the bytes the decompression reads)
struct Req {
  u32 jacc;
  u32 frame_lo, frame_hi;
  u32 interp;
  u32 amount;
  bool operator==(const Req& o) const {
    return jacc == o.jacc && frame_lo == o.frame_lo && frame_hi == o.frame_hi &&
           interp == o.interp && amount == o.amount;
  }
};

struct Slot {
  u32 gen = 0;
  s32 num_joints = -1;
  s32 nreq = -1;
  bool has = false;
  Req req[kMaxReqs];
  //! the head, then kTailBytes per request
  u8* data = nullptr;
  u32 cap = 0;
};

struct Cache {
  Slot slots[kSlots];
  u32 used = 0;

  static u32 index(u32 jc) { return ((jc >> 4) ^ (jc >> 12)) & (kSlots - 1); }

  Slot& slot(u32 jc) { return slots[index(jc)]; }

  static bool same(const Slot& s, s32 num_joints, s32 nreq, const Req* req) {
    if (s.gen != g_link_generation || s.num_joints != num_joints || s.nreq != nreq) {
      return false;
    }
    for (s32 i = 0; i < nreq; i++) {
      if (!(s.req[i] == req[i])) {
        return false;
      }
    }
    return true;
  }

  //! a new key for the slot (its stored frame, if any, is no longer valid)
  static void set_key(Slot& s, s32 num_joints, s32 nreq, const Req* req) {
    s.gen = g_link_generation;
    s.num_joints = num_joints;
    s.nreq = nreq;
    s.has = false;
    memcpy(s.req, req, sizeof(Req) * nreq);
  }

  //! room for a head of head_size bytes and nreq tails in the slot, or nullptr when over the
  //! budget
  u8* reserve(Slot& s, u32 head_size, s32 nreq) {
    const u32 size = head_size + kTailBytes * (u32)nreq;
    if (size > s.cap) {
      const u32 cap = (size + 255) & ~255u;
      if (used - s.cap + cap > kBudget) {
        return nullptr;
      }
      u8* p = (u8*)realloc(s.data, cap);
      if (!p) {
        return nullptr;
      }
      used = used - s.cap + cap;
      s.data = p;
      s.cap = cap;
    }
    return s.data;
  }
};

//! the game's cache, made on first use
inline Cache& cache_for(Cache*& c) {
  if (!c) {
    c = new Cache();
  }
  return *c;
}

//! head size of an output frame of num_joints joints (init-frame!'s quadwords)
inline u32 head_bytes(s64 num_joints) {
  return num_joints > 2 ? (u32)(128 + 48 * (num_joints - 2)) : 128u;
}
//! the first joint of the tail
inline s64 tail_begin(s64 num_joints) {
  return num_joints > 2 ? num_joints - 2 : 0;
}

}  // namespace Mips2C::anim_cache
