/*!
 * @file native_anim.cpp
 * (AI-assisted)
 * Native versions of GOAL functions of the joint decompressor (anim/joint.gc): decomp-request
 * (decomp-fixed, then decomp-frame for the frame and, when interpolating, the next one) and
 * finalize-joint-frame. They give the same results as goalc's C code of the GOAL functions
 * (test/mips2c_native/tests_anim.cpp): the same float operations in the same order, each on its
 * own (the GOAL code has no multiply-adds; clang would fuse an a + b * c written as one expression).
 *
 * The GOAL code runs three passes over the joints, each loading and storing every value it adds
 * to. Every transformq's trans, quat and scale come either from the fixed part or from the
 * frames (the same control bits pick it), so one pass per joint gives the same sums in the same
 * order, with the values kept in registers in between.
 */

#include <cstdlib>
#include <vector>

#include "common/log/log.h"

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/native_anim_cache.h"
#include "game/mips2c/native_goalc_ops.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace Mips2C::jak1::native {

namespace {

constexpr float kTransScale = 4.f;
constexpr float kQuatScale = 0.000030517578125f;  // QUAT_SCALE
constexpr float kScaleScale = 0.000244140625f;    // SCALE_SCALE

// GOAL memory through a base pointer kept in a register (gload / gstore read g_ee_main_mem again
// after every store, since a store could change it as far as the compiler knows)
template <typename T>
inline T ld(const u8* m, u32 addr) {
  T v;
  memcpy(&v, m + addr, sizeof(T));
  return v;
}
template <typename T>
inline void st(u8* m, u32 addr, T v) {
  MIPS2C_NATIVE_LOG_STORE(addr, (u32)sizeof(T));
  memcpy(m + addr, &v, sizeof(T));
}
inline void ld4(float out[4], const u8* m, u32 addr) {
  memcpy(out, MIPS2C_ASSUME_ALIGNED(m + addr, 16), 16);
}
inline void st4(u8* m, u32 addr, const float in[4]) {
  MIPS2C_NATIVE_LOG_STORE(addr, 16);
  memcpy(MIPS2C_ASSUME_ALIGNED(m + addr, 16), in, 16);
}

inline float s16f(const u8* m, u32 addr) {
  return (float)(s32)ld<s16>(m, addr);
}

//! x < 0.0 as goalc compares with a constant (gc_fcmp_k, GOALC_ABI_VERSION 5): negative with a
//! nonzero exponent, or (AI-assisted) a NaN of either sign, like the x86 backend's compare
inline bool below_zero(float x) {
  const u32 b = f2u(x);
  return b > 0x807fffffu || (s32)b > (s32)0x7f800000u;
}

//! the data streams of one part (fixed part or frame) of the animation
struct Stream {
  u32 d64, d32, d16;
  float amount;

  void init(const u8* m, u32 base, u32 offsets, float amt) {
    d64 = base + ld<u32>(m, offsets);
    d32 = base + ld<u32>(m, offsets + 4);
    d16 = base + ld<u32>(m, offsets + 8);
    amount = amt;
  }

  //! add-scaled-matrix! of the next matrix
  void matrix(u8* m, u32 dst) {
    float d[16];
    float src[16];
    for (int i = 0; i < 16; i += 4) {
      ld4(d + i, m, dst + 4 * i);
    }
    for (int i = 0; i < 16; i++) {
      src[i] = ld<float>(m, d64 + 4 * i);
    }
    for (int i = 0; i < 16; i++) {
      const float p = amount * src[i];
      d[i] = d[i] + p;
    }
    for (int i = 0; i < 16; i += 4) {
      st4(m, dst + 4 * i, d + i);
    }
    d64 += 64;
  }

  void trans(const u8* m, float t[3], bool big) {
    if (big) {
      const float x = ld<float>(m, d64);
      const float y = ld<float>(m, d64 + 4);
      const float z = ld<float>(m, d32);
      d64 += 8;
      d32 += 4;
      const float px = amount * x;
      const float py = amount * y;
      const float pz = amount * z;
      t[0] = t[0] + px;
      t[1] = t[1] + py;
      t[2] = t[2] + pz;
    } else {
      const float x = s16f(m, d32);
      const float y = s16f(m, d32 + 2);
      const float z = s16f(m, d16);
      d32 += 4;
      d16 += 2;
      float px = amount * x;
      float py = amount * y;
      float pz = amount * z;
      px = px * kTransScale;
      py = py * kTransScale;
      pz = pz * kTransScale;
      t[0] = t[0] + px;
      t[1] = t[1] + py;
      t[2] = t[2] + pz;
    }
  }

  void quat(const u8* m, float q[4]) {
    float t[4];
    for (int i = 0; i < 4; i++) {
      t[i] = s16f(m, d64 + 2 * i);
    }
    d64 += 8;
    // ((x x' + y y') + z z') + w w': the sign that keeps the sum on the same hemisphere
    const float p0 = t[0] * q[0];
    const float p1 = t[1] * q[1];
    const float p2 = t[2] * q[2];
    const float p3 = t[3] * q[3];
    float dot = p0 + p1;
    dot = dot + p2;
    dot = dot + p3;
    if (below_zero(dot)) {
      for (int i = 0; i < 4; i++) {
        t[i] = -t[i];
      }
    }
    for (int i = 0; i < 4; i++) {
      float p = amount * t[i];
      p = p * kQuatScale;
      q[i] = q[i] + p;
    }
  }

  void scale(const u8* m, float s[3]) {
    const float x = s16f(m, d32);
    const float y = s16f(m, d32 + 2);
    const float z = s16f(m, d16);
    d32 += 4;
    d16 += 2;
    float px = amount * x;
    float py = amount * y;
    float pz = amount * z;
    px = px * kScaleScale;
    py = py * kScaleScale;
    pz = pz * kScaleScale;
    s[0] = s[0] + px;
    s[1] = s[1] + py;
    s[2] = s[2] + pz;
  }
};

//! the stream positions of a request where a joint range starts (anim_cache tail)
struct RequestState {
  Stream fx, f0, f1;
  u32 fixed;
  u32 two;
};
static_assert(sizeof(RequestState) <= anim_cache::kTailBytes);

//! the request's contribution to transformqs [begin, end)
void decomp_joints(u8* m, u32 out, s64 begin, s64 end, RequestState& r) {
  u32 tq = out + 128 + 48 * (u32)begin;
  for (s64 i = begin; i < end; i++, tq += 48) {
    const u32 ctrl = (ld<u32>(m, r.fixed + 4 * (u32)(i >> 3)) >> (4 * (i & 7))) & 0xf;
    float t[3] = {ld<float>(m, tq), ld<float>(m, tq + 4), ld<float>(m, tq + 8)};
    if (ctrl & 1) {
      r.f0.trans(m, t, ctrl & 8);
      if (r.two) {
        r.f1.trans(m, t, ctrl & 8);
      }
    } else {
      r.fx.trans(m, t, ctrl & 8);
    }
    st<float>(m, tq, t[0]);
    st<float>(m, tq + 4, t[1]);
    st<float>(m, tq + 8, t[2]);
    float q[4];
    ld4(q, m, tq + 16);
    if (ctrl & 2) {
      r.f0.quat(m, q);
      if (r.two) {
        r.f1.quat(m, q);
      }
    } else {
      r.fx.quat(m, q);
    }
    st4(m, tq + 16, q);
    float s[3] = {ld<float>(m, tq + 32), ld<float>(m, tq + 36), ld<float>(m, tq + 40)};
    if (ctrl & 4) {
      r.f0.scale(m, s);
      if (r.two) {
        r.f1.scale(m, s);
      }
    } else {
      r.fx.scale(m, s);
    }
    st<float>(m, tq + 32, s[0]);
    st<float>(m, tq + 36, s[1]);
    st<float>(m, tq + 40, s[2]);
  }
}

/*!
 * (decomp-request output-frame num-joints request): adds the request's animation (fixed part,
 * frame, and the next frame weighted by frame-interp) times its amount to the output frame.
 * With save, the stream positions at joint save_at go there.
 */
void decomp_request(u8* m, u32 out, s64 num_joints, u32 req, s64 save_at, RequestState* save) {
  RequestState r;
  const u32 jacc = ld<u32>(m, req);
  const u32 frame = (u32)ld<u64>(m, req + 8);
  const float interp = ld<float>(m, req + 16);
  const float amount = ld<float>(m, req + 20);
  // the fixed part: a joint-anim-compressed-fixed, whose header is also the frames' header
  r.fixed = ld<u32>(m, jacc + 12);
  const u32 mbits = ld<u32>(m, r.fixed + 60);
  // (!= 0.0 frame-interp) on the bits, like goalc: denormals count as zero
  r.two = (f2u(interp) & 0x7f800000u) != 0;

  r.fx.init(m, r.fixed + 80, r.fixed + 64, amount);
  const float w0 = 1.f - interp;
  const u32 cur = ld<u32>(m, jacc + 16 + 4 * frame);
  r.f0.init(m, cur + 16, cur, w0 * amount);
  if (r.two) {
    const u32 next = ld<u32>(m, jacc + 16 + 4 * (frame + 1));
    r.f1.init(m, next + 16, next, interp * amount);
  } else {
    r.f1 = Stream{};
  }

  // matrices (the GOAL passes do these before their joints; they are separate outputs)
  if (!(mbits & 1)) {
    r.fx.matrix(m, out);
  }
  if (!(mbits & 2)) {
    r.fx.matrix(m, out + 64);
  }
  if (mbits & 1) {
    r.f0.matrix(m, out);
  }
  if (mbits & 2) {
    r.f0.matrix(m, out + 64);
  }
  if (r.two) {
    if (mbits & 1) {
      r.f1.matrix(m, out);
    }
    if (mbits & 2) {
      r.f1.matrix(m, out + 64);
    }
  }

  if (save) {
    const s64 mid = save_at < num_joints ? save_at : num_joints;
    decomp_joints(m, out, 0, mid, r);
    *save = r;
    decomp_joints(m, out, mid, num_joints, r);
  } else {
    decomp_joints(m, out, 0, num_joints, r);
  }
}

u64 decomp_request_impl(const NativeArgs& args) {
  // (C mode passes the low half: the count is small)
  decomp_request(g_ee_main_mem, (u32)args.a[0], (s32)args.a[1], (u32)args.a[2], 0, nullptr);
  return 0;
}

//! quaternion-normalize!: q * (1 / sqrt(((q.w^2 + q.z^2) + q.y^2) + q.x^2))
inline void normalize(float q[4]) {
  float sq[4];
  for (int i = 0; i < 4; i++) {
    sq[i] = q[i] * q[i];
  }
  float len2 = sq[3] + sq[2];
  len2 = len2 + sq[1];
  len2 = len2 + sq[0];
  const float inv = 1.f / __builtin_sqrtf(len2);
  for (int i = 0; i < 4; i++) {
    q[i] = q[i] * inv;
  }
}

/*!
 * (finalize-joint-frame output-frame num-joints): trans.w and scale.w = 1, quaternion
 * normalized, for the num-joints - 2 transformqs.
 */
void finalize_frame(u8* m, u32 out, s64 num_joints) {
  u32 tq = out + 128;
  // two at a time: one's square root and divide overlap the other's multiplies
  s64 i = 0;
  for (; i + 1 < num_joints - 2; i += 2, tq += 96) {
    float q0[4], q1[4];
    ld4(q0, m, tq + 16);
    ld4(q1, m, tq + 64);
    normalize(q0);
    normalize(q1);
    st<float>(m, tq + 12, 1.f);
    st<float>(m, tq + 44, 1.f);
    st4(m, tq + 16, q0);
    st<float>(m, tq + 60, 1.f);
    st<float>(m, tq + 92, 1.f);
    st4(m, tq + 64, q1);
  }
  if (i < num_joints - 2) {
    float q[4];
    ld4(q, m, tq + 16);
    normalize(q);
    st<float>(m, tq + 12, 1.f);
    st<float>(m, tq + 44, 1.f);
    st4(m, tq + 16, q);
  }
}

u64 finalize_frame_impl(const NativeArgs& args) {
  finalize_frame(g_ee_main_mem, (u32)args.a[0], (s32)args.a[1]);
  return 0;
}

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

// joint-control (basic: offsets from the object)
constexpr u32 kJcStatus = 0;           // janim-status, uint16
constexpr u32 kJcActiveChannels = 20;  // int32
constexpr u32 kJcChannel = 44;         // joint-control-channel, 48 bytes each
constexpr u16 kStatusSpool = 0x20;
// joint-control-channel
constexpr u32 kChCommand = 4;      // symbol
constexpr u32 kChFrameInterp = 8;  // float
constexpr u32 kChFrameGroup = 12;  // art-joint-anim (frames at +40)
constexpr u32 kChFrameNum = 16;    // float
constexpr u32 kChInspector = 44;   // float
// new-joint-decompressor-work
constexpr u32 kWorkNumRequests = 768;  // int32; requests (32 bytes each) at 0
constexpr u32 kWorkBlendStack = 784;   // blend-tree-stack-frame, 96 bytes each (24 weights)
constexpr u32 kWorkSize = kWorkBlendStack + 4 * 96;
constexpr u32 kMinWeight = 0x3a83126fu;  // 0.001

//! blend stack frame k (k may be negative: the GOAL code's address)
inline u32 blend_frame(u32 work, s64 k) {
  return work + kWorkBlendStack + (u32)(96 * (s32)k);
}

anim_cache::Cache* g_cache = nullptr;

/*!
 * The output frame from the requests in the work area: process-requests! and finalize-frame!,
 * through the frame cache when the joint-control may use it.
 */
void frame_from_requests(u8* m, u32 dst, s64 num_joints, u32 jc, u32 work) {
  using namespace anim_cache;
  const s32 nreq = ld<s32>(m, work + kWorkNumRequests);
  const u32 head = head_bytes(num_joints);
  const s64 tail_at = tail_begin(num_joints);
  Slot* slot = nullptr;
  Req key[kMaxReqs];
  bool same = false;
  if (nreq >= 0 && nreq <= kMaxReqs && head <= kMaxHead &&
      !(ld<u16>(m, jc + kJcStatus) & kStatusSpool)) {
    for (s32 i = 0; i < nreq; i++) {
      const u32 req = work + 32 * (u32)i;
      key[i] = {ld<u32>(m, req), ld<u32>(m, req + 8), ld<u32>(m, req + 12), ld<u32>(m, req + 16),
                ld<u32>(m, req + 20)};
    }
    slot = &cache_for(g_cache).slot(jc);
    same = Cache::same(*slot, (s32)num_joints, nreq, key);
    if (same && slot->has) {
      MIPS2C_NATIVE_LOG_STORE(dst, head);
      memcpy(m + dst, slot->data, head);
      for (s32 i = 0; i < nreq; i++) {
        RequestState r;
        memcpy(&r, slot->data + head + kTailBytes * (u32)i, sizeof(r));
        decomp_joints(m, dst, tail_at, num_joints, r);
      }
      st<u32>(m, work + kWorkNumRequests, 0);
      return;
    }
  }

  // init-frame!
  {
    const float zero[4] = {0, 0, 0, 0};
    for (u32 p = dst; p != dst + head; p += 16) {
      st4(m, p, zero);
    }
  }
  // process-requests! (the count read every time, like the GOAL loop)
  RequestState saved[kMaxReqs];
  const bool store = same;
  for (s32 i = 0; i < ld<s32>(m, work + kWorkNumRequests); i++) {
    decomp_request(m, dst, num_joints, work + 32 * (u32)i, tail_at, store ? &saved[i] : nullptr);
  }
  st<u32>(m, work + kWorkNumRequests, 0);
  finalize_frame(m, dst, num_joints);

  if (slot) {
    if (!same) {
      Cache::set_key(*slot, (s32)num_joints, nreq, key);
    } else if (u8* data = g_cache->reserve(*slot, head, nreq)) {
      // the head as the next calls start from it: the tail is added to whatever is there
      memcpy(data, m + dst, head);
      for (s32 i = 0; i < nreq; i++) {
        memcpy(data + head + kTailBytes * (u32)i, &saved[i], sizeof(RequestState));
      }
      slot->has = true;
    }
  }
}

//! the whole new-joint-decompressor; false (nothing written) where the GOAL code would (break!)
bool decompress(u32 dst, s64 num_joints, u32 jc, u32 work) {
  static const u32 sym_push = sym_addr("push");
  static const u32 sym_blend = sym_addr("blend");
  static const u32 sym_push1 = sym_addr("push1");
  static const u32 sym_stack = sym_addr("stack");
  u8* const m = g_ee_main_mem;
  const s64 num_channels = ld<s32>(m, jc + kJcActiveChannels);
  if (num_channels > 24) {
    return false;
  }
  {
    s64 top = 0;
    for (s64 i = 0; i < num_channels; i++) {
      const u32 cmd = ld<u32>(m, jc + kJcChannel + 48 * (u32)i + kChCommand);
      if (cmd == sym_push) {
        if (top > 4) {
          return false;
        }
        top++;
      } else if (cmd == sym_stack) {
        top--;
      }
    }
  }

  // eval-blend-tree!
  s64 top = 0;
  for (s64 i = 0; i < num_channels; i++) {
    const u32 chan = jc + kJcChannel + 48 * (u32)i;
    const u32 cmd = ld<u32>(m, chan + kChCommand);
    if (cmd == sym_push) {
      const u32 f = blend_frame(work, top);
      const float zero[4] = {0, 0, 0, 0};
      for (int q = 0; q < 6; q++) {
        st4(m, f + 16 * q, zero);
      }
      st<float>(m, f + 4 * (u32)i, 1.f);
      top++;
    } else if (cmd == sym_blend || cmd == sym_push1) {
      const u32 f = blend_frame(work, top - 1);
      const float nw = ld<float>(m, chan + kChFrameInterp);
      const float ow = 1.f - nw;
      for (int q = 0; q < 6; q++) {
        float v[4];
        ld4(v, m, f + 16 * q);
        for (int l = 0; l < 4; l++) {
          v[l] = v[l] * ow;
        }
        st4(m, f + 16 * q, v);
      }
      const float w = ld<float>(m, f + 4 * (u32)i);
      st<float>(m, f + 4 * (u32)i, w + nw);
    } else if (cmd == sym_stack) {
      const u32 f0 = blend_frame(work, top - 1);
      const u32 f1 = blend_frame(work, top - 2);
      const float w0 = ld<float>(m, chan + kChFrameInterp);
      const float w1 = 1.f - w0;
      for (int q = 0; q < 6; q++) {
        float a[4], b[4];
        ld4(a, m, f0 + 16 * q);
        ld4(b, m, f1 + 16 * q);
        for (int l = 0; l < 4; l++) {
          a[l] = a[l] * w0;
          b[l] = b[l] * w1;
          a[l] = a[l] + b[l];
        }
        st4(m, f1 + 16 * q, a);
      }
      top--;
    }
  }

  // output-blend-tree!
  const u32 weights = work + kWorkBlendStack;
  for (s64 i = 0; i < num_channels; i++) {
    st<float>(m, jc + kJcChannel + 48 * (u32)i + kChInspector, ld<float>(m, weights + 4 * (u32)i));
  }

  // build-requests!
  st<u32>(m, work + kWorkNumRequests, 0);
  for (s64 i = 0; i < num_channels; i++) {
    const float w = ld<float>(m, weights + 4 * (u32)i);
    if (goalc_ops::fcmp_xk(w, kMinWeight, goalc_ops::GC_FLE)) {
      continue;
    }
    const u32 chan = jc + kJcChannel + 48 * (u32)i;
    const s32 n = ld<s32>(m, work + kWorkNumRequests);
    const u32 req = work + (u32)((s64)n << 5);
    const u32 jacc = ld<u32>(m, ld<u32>(m, chan + kChFrameGroup) + 40);
    const float frame_num = ld<float>(m, chan + kChFrameNum);
    s64 base = goalc_ops::f2i(frame_num);
    float frac = frame_num - goalc_ops::i2f(base);
    const s64 last = (s64)((u64)ld<u32>(m, jacc) - 1);
    st<u32>(m, work + kWorkNumRequests, (u32)((s64)n + 1));
    if (!(base < last)) {
      frac = 0.f;
      base = last;
    }
    st<u32>(m, req, jacc);
    st<u64>(m, req + 8, (u64)base);
    st<float>(m, req + 16, frac);
    st<float>(m, req + 20, w);
  }

  frame_from_requests(m, dst, num_joints, jc, work);
  return true;
}

#if MIPS2C_WRITE_LOG
// verify mode (host, OPENGOAL_MIPS2C_VERIFY=1): the native on a copy of what it writes, then the
// GOAL code (the nested call returns #f), compared
int g_verify = -1;
bool g_nested = false;
u64 g_verify_calls = 0, g_verify_goal_only = 0, g_verify_mismatches = 0;

bool anim_verify_on() {
  if (g_verify < 0) {
    const char* v = getenv("OPENGOAL_MIPS2C_VERIFY");
    g_verify = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (g_verify) {
      atexit([] {
        lg::info("anim verify: {} calls ({} left to the GOAL code), {} mismatches", g_verify_calls,
                 g_verify_goal_only, g_verify_mismatches);
      });
    }
  }
  return g_verify == 1;
}

void anim_verify(const NativeArgs& args, u32 dst, s64 num_joints, u32 jc, u32 work) {
  struct Region {
    u32 addr, size;
    const char* name;
  };
  u8* const m = g_ee_main_mem;
  const s64 num_channels = ld<s32>(m, jc + kJcActiveChannels);
  const s64 qwc = num_joints > 2 ? 8 + 3 * (num_joints - 2) : 8;
  const s64 tq_end = 128 + 48 * (num_joints > 0 ? num_joints : 0);
  const u32 out_size = (u32)(16 * qwc > tq_end ? 16 * qwc : tq_end);
  const Region regions[3] = {
      {dst, out_size, "output frame"},
      {work, kWorkSize, "work"},
      {jc + kJcChannel, (u32)(48 * (num_channels > 0 ? num_channels : 0)), "channels"}};
  std::vector<u8> before[3], native[3];
  for (int r = 0; r < 3; r++) {
    before[r].assign(m + regions[r].addr, m + regions[r].addr + regions[r].size);
  }
  const bool done = decompress(dst, num_joints, jc, work);
  for (int r = 0; r < 3; r++) {
    native[r].assign(m + regions[r].addr, m + regions[r].addr + regions[r].size);
    memcpy(m + regions[r].addr, before[r].data(), regions[r].size);
  }
  static const u32 goal_fn_sym = sym_addr("new-joint-decompressor");
  g_nested = true;
  native_call_goal(gload<u32>(goal_fn_sym), args.a, args);
  g_nested = false;
  g_verify_calls++;
  if (!done) {
    g_verify_goal_only++;
    return;
  }
  for (int r = 0; r < 3; r++) {
    const u8* g = m + regions[r].addr;
    if (memcmp(g, native[r].data(), regions[r].size)) {
      g_verify_mismatches++;
      if (g_verify_mismatches <= 10) {
        u32 k = 0;
        while (g[k] == native[r][k]) {
          k++;
        }
        u32 gw, nw;
        memcpy(&gw, g + (k & ~3u), 4);
        memcpy(&nw, native[r].data() + (k & ~3u), 4);
        lg::error("anim verify MISMATCH (call {}, {} joints, {} channels): {} byte {}: GOAL "
                  "{:08x} native {:08x}",
                  g_verify_calls, num_joints, num_channels, regions[r].name, k, gw, nw);
      }
      break;
    }
  }
}
#endif

/*!
 * (pc-new-joint-decompressor dst num-joints jc) -> symbol: new-joint-decompressor's work. #f when
 * the GOAL code must run: where it would (break!), or when turned off (mips2c_native_off).
 */
u64 new_joint_decompressor_impl(const NativeArgs& args) {
  static const bool off = native_turned_off("pc-new-joint-decompressor");
  if (off) {
    return args.st;
  }
  static const u32 work_sym = sym_addr("*new-joint-decompressor-work*");
  const u32 dst = (u32)args.a[0];
  // (C mode passes the low half: the count is small)
  const s64 num_joints = (s32)(u32)args.a[1];
  const u32 jc = (u32)args.a[2];
  const u32 work = gload<u32>(work_sym);
#if MIPS2C_WRITE_LOG
  if (anim_verify_on()) {
    if (g_nested) {
      return args.st;
    }
    anim_verify(args, dst, num_joints, jc, work);
    return args.st + 8;
  }
#endif
  return decompress(dst, num_joints, jc, work) ? args.st + 8 : args.st;
}

}  // namespace

// GOAL code calls them directly (MIPS2C_NATIVE_IMPL_GOAL): once per animation request / per
// animated process and frame
const NativeImpl anim_decomp_request =
    MIPS2C_NATIVE_IMPL_GOAL(decomp_request_impl, NATIVE_V0_UNDEFINED);
const NativeImpl anim_finalize_frame =
    MIPS2C_NATIVE_IMPL_GOAL(finalize_frame_impl, NATIVE_V0_UNDEFINED);
const NativeImpl new_joint_decompressor =
    MIPS2C_NATIVE_IMPL_GOAL(new_joint_decompressor_impl, NATIVE_V0_UNDEFINED);

}  // namespace Mips2C::jak1::native

// GOAL functions with only a native version (def-mips2c in joint.gc; the GOAL versions are
// decomp-request and finalize-joint-frame, which joint.gc runs with anim_native = off).
namespace Mips2C::jak1 {
namespace pc_anim_decomp_request {
void link() {
  gLinkedFunctionTable.reg("pc-anim-decomp-request", native::anim_decomp_request.as_exec, 64,
                           &native::anim_decomp_request);
}
}  // namespace pc_anim_decomp_request
namespace pc_anim_finalize_frame {
void link() {
  gLinkedFunctionTable.reg("pc-anim-finalize-frame", native::anim_finalize_frame.as_exec, 64,
                           &native::anim_finalize_frame);
}
}  // namespace pc_anim_finalize_frame
namespace pc_new_joint_decompressor {
void link() {
  gLinkedFunctionTable.reg("pc-new-joint-decompressor", native::new_joint_decompressor.as_exec, 64,
                           &native::new_joint_decompressor);
}
}  // namespace pc_new_joint_decompressor
}  // namespace Mips2C::jak1
