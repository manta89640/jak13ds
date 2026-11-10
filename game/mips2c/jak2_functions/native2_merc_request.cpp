/*!
 * @file native2_merc_request.cpp
 * (AI-assisted)
 * Jak 2: pc-merc-draw-request (foreground.gc, the PC port's merc draw request for one object and
 * texture index) in C++. The GOAL function calls pc-merc-draw-request-native first and runs its own
 * code only when that returns 0.
 *
 * goalc's C of the GOAL function (out/jak2/cmod, the foreground module): the same stores in the
 * same order into the DMA buffer (the matrix pointers write 4 bytes of their quadword, like the GOAL
 * code), with charp<-string, quad-copy! (qmem-copy<-!) and mem-copy! inline while their symbols hold
 * what they held at the first call. A matrix number >= 128 is the GOAL code's (break!): the native
 * leaves that request to the GOAL code (checked before anything is written). Verify mode
 * (OPENGOAL_MIPS2C_VERIFY=1) runs the GOAL code on every call and compares (native_verify_inline:
 * the DMA data, the returned end pointer).
 */

#include <cstring>

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native2.h"

namespace Mips2C::jak2::native2 {

namespace {

//! a symbol's value address and the function it held at the first call
struct FnSym {
  const char* name;
  u32 val = 0;
  u32 fn = 0;
  bool resolved = false;
  explicit FnSym(const char* n) : name(n) {}
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      val = sym_value_addr(name);
      fn = gload<u32>(val);
    }
    return fn != 0 && gload<u32>(val) == fn;
  }
};

FnSym s_charp("charp<-string");
FnSym s_quad_copy("quad-copy!");
FnSym s_qmem_copy("qmem-copy<-!");
FnSym s_mem_copy("mem-copy!");
FnSym s_goal_fn("pc-merc-draw-request");

u32 foreground_val() {
  static const u32 v = sym_value_addr("*foreground*");
  return v;
}

// draw-control
constexpr u32 kDcStatus = 4;        // u16
constexpr u32 kDcGlobalEffect = 7;  // u8
constexpr u32 kDcMgeo = 16;
constexpr u32 kDcLodSet = 0x1c;  // lod entries, 8 bytes each, geo first
constexpr u32 kDcCurLod = 78;    // s8
// *foreground* merc-bucket-info
constexpr u32 kFgLight = 0x950;
constexpr u32 kFgEffect = 0x9cc;  // 8 bytes each: color-fade u32, merc-path, ignore-alpha, disable-draw
// merc-ctrl
constexpr u32 kMcEffectCount = 52;  // u32
constexpr u32 kMcEffect = 0x9c;     // 32 bytes each

//! what the effects drawn with this texture index give: their bits and the matrix slot list
struct Slots {
  u8 list[128];  // the matrix numbers in the order they are first used, 0xff after them
  u32 count;
  u64 enable_mask, ignore_alpha_mask;
};

//! False: a matrix number >= 128 (the GOAL code's (break!)).
bool collect_slots(u32 mc, s64 tex_idx, u32 fg, Slots* out) {
  u8 transferred[128];
  memset(transferred, 0, sizeof(transferred));
  memset(out->list, 0xff, sizeof(out->list));
  out->count = 0;
  out->enable_mask = 0;
  out->ignore_alpha_mask = 0;
  const s64 effects = (s64)gload<u32>(mc + kMcEffectCount);
  for (s64 e = 0; e < effects; e++) {
    const u32 bi = (u32)(((u64)e << 3) + kFgEffect + fg);
    const u32 eff = (u32)(((u64)e << 5) + kMcEffect + mc);
    if (gload<u8>(bi + 4) != 0 || gload<u8>(bi + 6) != 0 ||
        tex_idx != (s64)gload<u8>(eff + 26)) {
      continue;
    }
    const u64 bit = e > 0 ? (u64)1 << (e & 63) : (u64)((s64)1 >> ((0 - e) & 63));
    out->enable_mask |= bit;
    if (gload<u8>(bi + 5) != 0) {
      out->ignore_alpha_mask |= bit;
    }
    u32 frag = gload<u32>(eff + 4);
    const u32 frags = gload<u16>(eff + 18);
    for (u32 f = 0; f < frags; f++) {
      const u32 n = gload<u8>(frag + 3);
      for (u32 m = 0; m < n; m++) {
        const u32 mat = gload<u8>(frag + 4 + 2 * m);
        if (mat >= 128) {
          return false;
        }
        if (transferred[mat] == 0) {
          transferred[mat] = 1;
          out->list[out->count++] = (u8)mat;
        }
      }
      frag = frag + 2 * n + 4;
    }
  }
  return true;
}

u32 merc_draw_request(u32 dc,
                      u32 dma,
                      u32 matrix_buf,
                      const Slots& slots,
                      u32 mc,
                      u32 fg,
                      u32 update_verts,
                      u32 blercs,
                      u32 blerc_sym,
                      u32 false_sym) {
  const u32 start = dma;
  gstore<u64>(start, 0x10000000ull);
  gstore<u32>(start + 8, 0);
  gstore<u32>(start + 12, 0x8000000u);
  dma = start + 16;
  // NAME: charp<-string
  {
    const char* name = (const char*)gptr(gload<u32>(gload<u32>(dc + kDcMgeo) + 4) + 4);
    gstore_bytes(dma, name, (u32)strlen(name) + 1);
  }
  dma += 16 * 8;
  u64 qwc = 8;
  // LIGHTS: quad-copy! of 7 quadwords
  gstore_bytes(dma, gptr(fg + kFgLight), 16 * 7);
  dma += 16 * 7;
  qwc += 7;
  // the matrix slot list, then a pointer per slot (4 bytes of its quadword)
  gstore_bytes(dma, slots.list, 128);
  dma += 16 * 8;
  qwc += 8;
  for (u32 k = 0; k < slots.count; k++) {
    gstore<u32>(dma, (u32)((u64)(s64)(s32)((u32)0x80 * (u32)slots.list[k]) + matrix_buf));
    dma += 16;
  }
  qwc += slots.count;
  const u32 effects = gload<u32>(mc + kMcEffectCount);
  // flags
  const u32 flags = dma;
  u8 bits = 0;
  if (update_verts != false_sym) {
    bits |= update_verts == blerc_sym ? 4 : 1;
  }
  if (gload<u16>(dc + kDcStatus) & 0x2000) {
    bits |= 2;
  }
  if (gload<u8>(dc + kDcGlobalEffect) & 8) {
    bits |= 8;
  }
  gstore<u64>(flags, slots.enable_mask);
  gstore<u64>(flags + 8, slots.ignore_alpha_mask);
  gstore<u8>(flags + 16, (u8)effects);
  gstore<u8>(flags + 17, bits);
  dma += 16 * 2;
  qwc += 2;
  if (update_verts == blerc_sym) {
    gstore_bytes(dma, gptr(blercs), 40 * 4);
    dma += 40 * 4;
    qwc += 10;
  }
  // fades
  for (u32 i = 0; i < effects; i++) {
    gstore<u32>(dma + 4 * i, gload<u32>(fg + kFgEffect + 8 * i));
  }
  const u32 n = (effects + 3) >> 2;
  dma += 16 * n;
  qwc += n;
  // merc pointers
  for (u32 i = 0; i < effects; i++) {
    gstore<u32>(dma + 4 * i, mc + kMcEffect + 32 * i);
  }
  dma += 16 * n;
  qwc += n;
  gstore<u64>(start, 0x10000000ull | ((qwc << 48) >> 48));
  return dma;
}

bool inline_ok() {
  return s_charp.unchanged() && s_quad_copy.unchanged() && s_qmem_copy.unchanged() &&
         s_mem_copy.unchanged();
}

}  // namespace

/*!
 * (pc-merc-draw-request-native dc dma-buf matrix-buf tex-idx update-verts blercs): the end of the
 * request, or 0 when the GOAL code must run.
 */
u64 pc_merc_draw_request_impl(const NativeArgs& args) {
  static int on = -1;
  if (on < 0) {
    on = native_turned_off("pc-merc-draw-request-native") ? 0 : 1;
  }
  if (on != 1 || !inline_ok()) {
    return 0;
  }
  const u32 dc = (u32)args.a[0];
  const u32 dma = (u32)args.a[1];
  const u32 matrix_buf = (u32)args.a[2];
  const s64 tex_idx = (s64)(s32)(u32)args.a[3];
  const u32 update_verts = (u32)args.a[4];
  const u32 blercs = (u32)args.a[5];
  const u32 fg = gload<u32>(foreground_val());
  const u32 mc = gload<u32>((u32)(((u64)(s64)gload<s8>(dc + kDcCurLod) << 3) + kDcLodSet + dc));
  Slots slots;
  if (!collect_slots(mc, tex_idx, fg, &slots)) {
    return 0;
  }
  static const u32 blerc_sym = ::jak2::intern_from_c("blerc").offset;
  const u32 false_sym = (u32)args.st;
#if MIPS2C_WRITE_LOG
  static bool nested = false;
  if (nested) {
    return 0;
  }
  if (native_verify_inline_on() && s_goal_fn.unchanged()) {
    // the most a request writes: header, name, lights, slots, 128 matrices, flags, blerc weights,
    // fades and pointers for up to 64 effects
    const u32 max_bytes = 16 * (1 + 8 + 7 + 8 + 128 + 2 + 10 + 16 + 16);
    return native_verify_inline(
        "pc-merc-draw-request", {{dma, max_bytes}},
        [&]() -> u64 {
          return merc_draw_request(dc, dma, matrix_buf, slots, mc, fg, update_verts, blercs,
                                   blerc_sym, false_sym);
        },
        [&]() -> u64 {
          nested = true;
          const u64 a[8] = {dc, dma, matrix_buf, (u64)tex_idx, update_verts, blercs, 0, 0};
          const u64 r = native_call_goal(s_goal_fn.fn, a, args);
          nested = false;
          return r;
        });
  }
#endif
  return merc_draw_request(dc, dma, matrix_buf, slots, mc, fg, update_verts, blercs, blerc_sym,
                           false_sym);
}

const NativeImpl pc_merc_draw_request_native = MIPS2C_NATIVE_IMPL_GOAL(pc_merc_draw_request_impl, 0);

}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::pc_merc_request_natives {
void link() {
  gLinkedFunctionTable.reg("pc-merc-draw-request-native",
                           native2::pc_merc_draw_request_native.as_exec, 64,
                           &native2::pc_merc_draw_request_native);
}
}  // namespace Mips2C::jak2::pc_merc_request_natives
