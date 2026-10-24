/*!
 * @file native_anim.cpp
 * (AI-assisted)
 * Jak 2: native version of the PC port's joint decompressor (anim/joint.gc new-joint-decompressor:
 * eval-blend-tree!, output-blend-tree!, build-requests!, init-frame!, process-requests! with
 * process-request! / decomp-fixed / decomp-frame / add-scaled-matrix!, and finalize-frame! with
 * quaternion-normalize!). Jak 3's joint.gc has the same GOAL code. Jak 1 has natives for the
 * request and the final normalize only (jak1_functions/native_anim.cpp); this does the whole call,
 * so the six method calls and their C-mode call glue go away too.
 *
 * Same results as goalc's C code of the GOAL functions: the same float operations in the same
 * order, each on its own (no multiply-adds: clang would fuse a + b * c written as one expression,
 * hence fp contract off below); float compares with a constant as goalc does them
 * (native_goalc_ops.h); the same GOAL memory addresses, also where the GOAL code reads or
 * writes outside the structures (a blend before any push uses blend stack frame -1, the joint
 * loops run num-joints transformqs, two more than init-frame! zeroes).
 *
 * (pc-new-joint-decompressor dst num-joints jc) -> symbol. #f when the GOAL code must run: when it
 * would stop at a (break!) (more than 24 channels or 5 pushed blend frames: checked before
 * anything is written), and for debugging (config.ini mips2c_native_off lists
 * pc-new-joint-decompressor or is all; host: OPENGOAL_ANIM_NATIVE=0 or OPENGOAL_MIPS2C_NATIVE=0).
 *
 * Verify mode (host, OPENGOAL_MIPS2C_VERIFY=1): every call runs the native on a copy of the memory
 * it touches, puts the memory back, calls new-joint-decompressor itself (the nested call returns
 * #f here, so the GOAL code runs) and compares: the output frame, the decompressor's work area,
 * its stats and the channels' inspector-amount bytes. "anim verify" lines, a summary at exit.
 */

#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/log/log.h"

#include "game/mips2c/jak2_functions/native_functions.h"
#include "game/mips2c/mips2c_native.h"
#include "game/mips2c/mips2c_private.h"
#include "game/mips2c/native_goalc_ops.h"

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace Mips2C::jak2::native {

namespace {

int g_anim_native = -1;  // -1: config not read yet

bool anim_native_on() {
  if (g_anim_native < 0) {
    g_anim_native = 1;
#ifdef __3DS__
    char list[256];
    if (ctr_config_get("mips2c_native_off", list, sizeof(list)) &&
        (strstr(list, "pc-new-joint-decompressor") || !strcmp(list, "all") ||
         !strcmp(list, "on") || !strcmp(list, "1") || !strcmp(list, "yes") ||
         !strcmp(list, "true"))) {
      g_anim_native = 0;
    }
#else
    const char* env = getenv("OPENGOAL_ANIM_NATIVE");
    if (env && env[0] == '0') {
      g_anim_native = 0;
    }
    const char* all = getenv("OPENGOAL_MIPS2C_NATIVE");
    if (all && all[0] == '0') {
      g_anim_native = 0;
    }
#endif
    lg::info("[anim] Jak 2 native new-joint-decompressor {}",
             g_anim_native ? "on" : "off (mips2c_native_off / OPENGOAL_*_NATIVE=0)");
  }
  return g_anim_native == 1;
}

using namespace ::Mips2C::goalc_ops;

// GOAL memory through a base pointer kept in a register
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

constexpr float kTransScale = 4.f;
constexpr float kQuatScale = 0.000030517578125f;  // QUAT_SCALE
constexpr float kScaleScale = 0.000244140625f;    // SCALE_SCALE
constexpr u32 kZero = 0;                          // 0.0
constexpr u32 kMinWeight = 0x3a83126fu;           // 0.001

// joint-control (basic: offsets from the object, the type at -4)
constexpr u32 kJcActiveChannels = 3;     // uint8
constexpr u32 kJcActiveFrameInterp = 17; // uint8
constexpr u32 kJcFloatChannels = 18;     // uint8
constexpr u32 kJcChannel = 60;           // joint-control-channel, 64 bytes each
// joint-control-channel
constexpr u32 kChFrameGroup = 4;   // art-joint-anim (frames at +40)
constexpr u32 kChFrameNum = 8;     // float
constexpr u32 kChFrameInterp = 32; // float 2
constexpr u32 kChInspector = 40;   // uint8
constexpr u32 kChCommand = 48;     // int64
// new-joint-decompressor-work
constexpr u32 kWorkNumRequests = 768;  // int32; requests (32 bytes each) at 0
constexpr u32 kWorkBlendStack = 784;   // blend-tree-stack-frame, 96 bytes each (24 weights)
constexpr u32 kWorkSize = kWorkBlendStack + 4 * 96;
// joint-control-command
constexpr u64 kCmdPush = 1, kCmdBlend = 2, kCmdPush1 = 19, kCmdStack = 36, kCmdFloat = 42;

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

  //! add-scaled-matrix! of the next matrix: dst[i] + scale * src[i]
  void matrix(u8* m, u32 dst) {
    float d[16];
    for (int i = 0; i < 16; i += 4) {
      ld4(d + i, m, dst + 4 * i);
    }
    for (int i = 0; i < 16; i++) {
      const float p = amount * ld<float>(m, d64 + 4 * i);
      d[i] = d[i] + p;
    }
    for (int i = 0; i < 16; i += 4) {
      st4(m, dst + 4 * i, d + i);
    }
    d64 += 64;
  }

  void trans(const u8* m, float t[3], bool big) {
    if (big) {
      const float px = amount * ld<float>(m, d64);
      const float py = amount * ld<float>(m, d64 + 4);
      const float pz = amount * ld<float>(m, d32);
      d64 += 8;
      d32 += 4;
      t[0] = t[0] + px;
      t[1] = t[1] + py;
      t[2] = t[2] + pz;
    } else {
      float px = amount * s16f(m, d32);
      float py = amount * s16f(m, d32 + 2);
      float pz = amount * s16f(m, d16);
      d32 += 4;
      d16 += 2;
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
    // ((x x' + y y') + z z') + w w'
    const float p0 = t[0] * q[0];
    const float p1 = t[1] * q[1];
    const float p2 = t[2] * q[2];
    const float p3 = t[3] * q[3];
    float dot = p0 + p1;
    dot = dot + p2;
    dot = dot + p3;
    // (when (< dot 0.0) ... (*! x -1.)): goalc branches on (>= dot 0.0)
    if (!fcmp_xk(dot, kZero, GC_FGE)) {
      for (int i = 0; i < 4; i++) {
        t[i] = t[i] * -1.f;
      }
    }
    for (int i = 0; i < 4; i++) {
      float p = amount * t[i];
      p = p * kQuatScale;
      q[i] = q[i] + p;
    }
  }

  void scale(const u8* m, float s[3]) {
    float px = amount * s16f(m, d32);
    float py = amount * s16f(m, d32 + 2);
    float pz = amount * s16f(m, d16);
    d32 += 4;
    d16 += 2;
    px = px * kScaleScale;
    py = py * kScaleScale;
    pz = pz * kScaleScale;
    s[0] = s[0] + px;
    s[1] = s[1] + py;
    s[2] = s[2] + pz;
  }
};

/*!
 * process-request!: decomp-fixed, decomp-frame of the frame and, when frame-interp isn't 0, of the
 * next one, added to the output frame. For a value of a joint, either the fixed part or the frames
 * have it (the same control bits pick it), so one pass over the joints gives the GOAL passes' sums
 * in the same order; the matrices come before the joints in each GOAL pass and are separate
 * outputs.
 */
void process_request(u8* m, u32 out, s64 num_joints, u32 req, u32 stats) {
  // stats: num-anims, num-joints
  st<u32>(m, stats + 4, (u32)(ld<s32>(m, stats + 4) + 1));
  st<u32>(m, stats + 8, (u32)((s64)ld<s32>(m, stats + 8) + (num_joints + 2)));

  const u32 jacc = ld<u32>(m, req);
  const u64 frame = ld<u64>(m, req + 8);
  const float interp = ld<float>(m, req + 16);
  const float amount = ld<float>(m, req + 20);
  // the fixed part: a joint-anim-compressed-fixed, whose header is also the frames' header
  const u32 fixed = ld<u32>(m, jacc + 12);
  const u32 mbits = ld<u32>(m, fixed + 60);
  // (!= 0.0 frame-interp): goalc's gc_fcmp(0.0, interp, GC_FEQ) skips the second frame
  const bool two = !fcmp_xk(interp, kZero, GC_FEQ);

  Stream fx, f0, f1;
  fx.init(m, fixed + 80, fixed + 64, amount);
  float w0 = 1.f - interp;
  w0 = w0 * amount;
  const u32 cur = ld<u32>(m, (u32)((frame << 2) + 16 + jacc));
  f0.init(m, cur + 16, cur, w0);
  if (two) {
    const u32 next = ld<u32>(m, (u32)(((1 + frame) << 2) + 16 + jacc));
    f1.init(m, next + 16, next, interp * amount);
  }

  if (!(mbits & 1)) {
    fx.matrix(m, out);
  }
  if (!(mbits & 2)) {
    fx.matrix(m, out + 64);
  }
  if (mbits & 1) {
    f0.matrix(m, out);
  }
  if (mbits & 2) {
    f0.matrix(m, out + 64);
  }
  if (two) {
    if (mbits & 1) {
      f1.matrix(m, out);
    }
    if (mbits & 2) {
      f1.matrix(m, out + 64);
    }
  }

  u32 tq = out + 128;
  for (s64 i = 0; i < num_joints; i++, tq += 48) {
    const u32 ctrl = (ld<u32>(m, fixed + 4 * (u32)(i >> 3)) >> (4 * (i & 7))) & 0xf;
    float t[3] = {ld<float>(m, tq), ld<float>(m, tq + 4), ld<float>(m, tq + 8)};
    if (ctrl & 1) {
      f0.trans(m, t, ctrl & 8);
      if (two) {
        f1.trans(m, t, ctrl & 8);
      }
    } else {
      fx.trans(m, t, ctrl & 8);
    }
    st<float>(m, tq, t[0]);
    st<float>(m, tq + 4, t[1]);
    st<float>(m, tq + 8, t[2]);
    float q[4];
    ld4(q, m, tq + 16);
    if (ctrl & 2) {
      f0.quat(m, q);
      if (two) {
        f1.quat(m, q);
      }
    } else {
      fx.quat(m, q);
    }
    st4(m, tq + 16, q);
    float s[3] = {ld<float>(m, tq + 32), ld<float>(m, tq + 36), ld<float>(m, tq + 40)};
    if (ctrl & 4) {
      f0.scale(m, s);
      if (two) {
        f1.scale(m, s);
      }
    } else {
      fx.scale(m, s);
    }
    st<float>(m, tq + 32, s[0]);
    st<float>(m, tq + 36, s[1]);
    st<float>(m, tq + 40, s[2]);
  }
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

//! the 6 quadwords (24 weights) of blend stack frame k (k may be -1: the GOAL code's address)
inline u32 blend_frame(u32 work, s64 k) {
  return work + kWorkBlendStack + (u32)(96 * (s32)k);
}

//! the whole new-joint-decompressor; false (nothing written) where the GOAL code would (break!)
bool decompress(u32 dst, s64 num_joints, u32 jc, u32 work, u32 stats) {
  u8* const m = g_ee_main_mem;
  const s64 num_channels =
      (s64)ld<u8>(m, jc + kJcActiveChannels) + (s64)ld<u8>(m, jc + kJcFloatChannels);
  if (num_channels > 24) {
    return false;
  }
  {
    s64 top = 0;
    for (s64 i = 0; i < num_channels; i++) {
      const u64 cmd = ld<u64>(m, jc + kJcChannel + 64 * (u32)i + kChCommand);
      if (cmd == kCmdPush) {
        if (top > 4) {
          return false;
        }
        top++;
      } else if (cmd == kCmdStack) {
        top--;
      }
    }
  }

  // stats num-calls
  st<u32>(m, stats, (u32)(ld<s32>(m, stats) + 1));

  // eval-blend-tree!
  const u32 afi = ld<u8>(m, jc + kJcActiveFrameInterp);
  s64 top = 0;
  for (s64 i = 0; i < num_channels; i++) {
    const u32 chan = jc + kJcChannel + 64 * (u32)i;
    const u64 cmd = ld<u64>(m, chan + kChCommand);
    if (cmd == kCmdPush) {
      const u32 f = blend_frame(work, top);
      const float zero[4] = {0, 0, 0, 0};
      for (int q = 0; q < 6; q++) {
        st4(m, f + 16 * q, zero);
      }
      st<float>(m, f + 4 * (u32)i, 1.f);
      top++;
    } else if (cmd == kCmdBlend || cmd == kCmdPush1 || cmd == kCmdFloat) {
      const u32 f = blend_frame(work, top - 1);
      const float nw = ld<float>(m, chan + kChFrameInterp + 4 * afi);
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
    } else if (cmd == kCmdStack) {
      const u32 f0 = blend_frame(work, top - 1);
      const u32 f1 = blend_frame(work, top - 2);
      const float w0 = ld<float>(m, chan + kChFrameInterp + 4 * afi);
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
    const float a = 255.f * ld<float>(m, weights + 4 * (u32)i);
    st<u8>(m, jc + kJcChannel + 64 * (u32)i + kChInspector, (u8)f2i(a));
  }

  // build-requests!
  st<u32>(m, work + kWorkNumRequests, 0);
  for (s64 i = 0; i < num_channels; i++) {
    const float w = ld<float>(m, weights + 4 * (u32)i);
    if (fcmp_xk(w, kMinWeight, GC_FLE)) {
      continue;
    }
    const u32 chan = jc + kJcChannel + 64 * (u32)i;
    const s32 n = ld<s32>(m, work + kWorkNumRequests);
    const u32 req = work + (u32)((s64)n << 5);
    const u32 jacc = ld<u32>(m, ld<u32>(m, chan + kChFrameGroup) + 40);
    const float frame_num = ld<float>(m, chan + kChFrameNum);
    s64 base = f2i(frame_num);
    float frac = frame_num - i2f(base);
    const s64 last = (s64)((u64)ld<u16>(m, jacc) - 1);
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

  // init-frame!
  {
    s64 qwc = 8;
    if (num_joints > 2) {
      qwc = qwc + (s64)(s32)(3 * (s32)(num_joints - 2));
    }
    const float zero[4] = {0, 0, 0, 0};
    const u32 end = dst + (u32)(qwc << 4);
    for (u32 p = dst; p != end; p += 16) {
      st4(m, p, zero);
    }
  }

  // process-requests!
  for (s64 i = 0; i < (s64)ld<s32>(m, work + kWorkNumRequests); i++) {
    process_request(m, dst, num_joints, work + (u32)(i << 5), stats);
  }
  st<u32>(m, work + kWorkNumRequests, 0);

  // finalize-frame!
  if (num_joints > 2) {
    u32 tq = dst + 128;
    for (s64 i = 0; i < num_joints - 2; i++, tq += 48) {
      st<float>(m, tq + 12, 1.f);
      st<float>(m, tq + 44, 1.f);
      float q[4];
      ld4(q, m, tq + 16);
      normalize(q);
      st4(m, tq + 16, q);
    }
  }
  return true;
}

#if MIPS2C_WRITE_LOG
#if defined(__APPLE__)
// (AI-assisted) the thread's retired instructions (macOS): the cost of both versions in verify mode
extern "C" int thread_selfcounts(int type, void* buf, size_t nbytes);
inline u64 thread_instructions() {
  u64 c[2] = {0, 0};
  thread_selfcounts(1, c, sizeof(c));
  return c[0];
}
#else
inline u64 thread_instructions() {
  return 0;
}
#endif
u64 g_instr_native = 0, g_instr_goal = 0;

// verify mode (host): see the top
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
        if (g_instr_native) {
          lg::info("anim verify: host instructions per call: native {:.0f}, GOAL {:.0f} ({:.2f}x)",
                   (double)g_instr_native / g_verify_calls, (double)g_instr_goal / g_verify_calls,
                   (double)g_instr_goal / (double)g_instr_native);
        }
      });
    }
  }
  return g_verify == 1;
}

struct Region {
  u32 addr, size;
  const char* name;
};

void anim_verify(const NativeArgs& args, u32 dst, s64 num_joints, u32 jc, u32 work, u32 stats) {
  u8* const m = g_ee_main_mem;
  const s64 num_channels =
      (s64)ld<u8>(m, jc + kJcActiveChannels) + (s64)ld<u8>(m, jc + kJcFloatChannels);
  // what the GOAL code can write: the output frame (init-frame!'s quadwords or num-joints
  // transformqs, whichever is more), the work area from blend frame -1 on, the stats, the channels
  const s64 qwc = num_joints > 2 ? 8 + 3 * (num_joints - 2) : 8;
  const s64 tq_end = 128 + 48 * (num_joints > 0 ? num_joints : 0);
  const u32 out_size = (u32)(16 * qwc > tq_end ? 16 * qwc : tq_end);
  const Region regions[4] = {
      {dst, out_size, "output frame"},
      {work, kWorkSize, "work"},
      {stats, 16, "stats"},
      {jc + kJcChannel, (u32)(64 * (num_channels > 0 ? num_channels : 0)), "channels"}};
  std::vector<u8> before[4], native[4];
  for (int r = 0; r < 4; r++) {
    before[r].assign(m + regions[r].addr, m + regions[r].addr + regions[r].size);
  }
  const u64 i0 = thread_instructions();
  const bool done = decompress(dst, num_joints, jc, work, stats);
  const u64 i1 = thread_instructions();
  for (int r = 0; r < 4; r++) {
    native[r].assign(m + regions[r].addr, m + regions[r].addr + regions[r].size);
    memcpy(m + regions[r].addr, before[r].data(), regions[r].size);
  }
  static const u32 goal_fn_sym = sym_val_addr("new-joint-decompressor");
  g_nested = true;
  const u64 i2 = thread_instructions();
  native_call_goal(gload<u32>(goal_fn_sym), args.a, args);
  const u64 i3 = thread_instructions();
  g_nested = false;
  g_instr_native += i1 - i0;
  g_instr_goal += i3 - i2;
  g_verify_calls++;
  if (!done) {
    g_verify_goal_only++;
    return;  // the GOAL code ran alone
  }
  for (int r = 0; r < 4; r++) {
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

}  // namespace

/*!
 * (pc-new-joint-decompressor dst num-joints jc): new-joint-decompressor's work. #t when done.
 */
u64 pc_new_joint_decompressor_impl(const NativeArgs& args) {
  if (!anim_native_on()) {
    return args.st;  // #f: the GOAL code runs
  }
  const u32 dst = (u32)args.a[0];
  // (C mode passes the low half: the count is small)
  const s64 num_joints = (s32)(u32)args.a[1];
  const u32 jc = (u32)args.a[2];
  static const u32 work_sym = sym_val_addr("*new-joint-decompressor-work*");
  static const u32 stats_sym = sym_val_addr("*new-joint-decompressor-stats*");
  const u32 work = gload<u32>(work_sym);
  const u32 stats = gload<u32>(stats_sym);
#if MIPS2C_WRITE_LOG
  if (anim_verify_on()) {
    if (g_nested) {
      return args.st;  // #f: the GOAL code runs (the reference)
    }
    anim_verify(args, dst, num_joints, jc, work, stats);
    return goal_true(args.st);
  }
#endif
  return decompress(dst, num_joints, jc, work, stats) ? goal_true(args.st) : args.st;
}

const NativeImpl pc_new_joint_decompressor =
    MIPS2C_NATIVE_IMPL_GOAL(pc_new_joint_decompressor_impl, NATIVE_V0_UNDEFINED);

}  // namespace Mips2C::jak2::native

namespace Mips2C::jak2::pc_new_joint_decompressor {
void link() {
  gLinkedFunctionTable.reg("pc-new-joint-decompressor", native::pc_new_joint_decompressor.as_exec,
                           64, &native::pc_new_joint_decompressor);
}
}  // namespace Mips2C::jak2::pc_new_joint_decompressor
