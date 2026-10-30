#include "reverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace snd {

namespace {

// libsd's preset table (LIBSD.IRX), in the register order dAPF1 dAPF2 vIIR vCOMB1-4 vWALL vAPF1
// vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 dLSAME dRSAME mLDIFF mRDIFF mLCOMB3 mRCOMB3
// mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN vRIN. Addresses in 8-byte units.
constexpr u16 kPresets[10][32] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
     0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
     0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5,
     0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4,
     0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135,
     0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904,
     0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305,
     0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB,
     0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0,
     0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA,
     0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1,
     0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
    {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6,
     0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D,
     0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD,
     0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE,
     0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD,
     0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE,
     0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371,
     0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1,
     0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};
// work area sizes, 8-byte units
constexpr u16 kWorkSize[10] = {0x2,    0x4d8,  0x3e8,  0x908,  0xdfc,
                               0x15bc, 0x1ed8, 0x3008, 0x3008, 0x780};

enum Reg {
  dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
  mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
  mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2,
  vLIN, vRIN,
};

// PCSX2's 39-tap half-band filter (its odd taps are 0 but the centre one, 16384), over b[0..38]:
// the even taps, symmetric
inline s32 FirEven(const s16* b) {
  return -(b[0] + b[38]) + 2 * (b[2] + b[36]) - 10 * (b[4] + b[34]) + 35 * (b[6] + b[32]) -
         103 * (b[8] + b[30]) + 266 * (b[10] + b[28]) - 616 * (b[12] + b[26]) +
         1332 * (b[14] + b[24]) - 2960 * (b[16] + b[22]) + 10246 * (b[18] + b[20]);
}

inline s32 Mul(s32 a, s32 b) {
  return (a * b) >> 15;
}

inline s32 Clamp16(s32 v) {
  return std::clamp<s32>(v, INT16_MIN, INT16_MAX);
}

}  // namespace

void Reverb::SetMode(u32 mode) {
  // libsd refuses modes past 9 and leaves everything as it was
  if (mode == mMode || mode > 9) {
    return;
  }
  mMode = mode;
  if (mode == 0) {
    std::vector<s16>().swap(mWork);
    return;
  }

  s32 r[32];
  for (int i = 0; i < 32; i++) {
    r[i] = kPresets[mode][i];
  }
  if (mode == 7 || mode == 8) {
    // libsd's echo and delay take the attribute's delay and feedback (the overlord passes 0)
    const s32 delay = 0, feedback = 0;
    const s32 d = delay + 1;
    r[mLSAME] = (d << 6) - r[dAPF1];
    r[mRSAME] = (d << 5) - r[dAPF2];
    r[dLSAME] = r[dRSAME] + (d << 5);
    r[mLCOMB1] = r[mRCOMB1] + (d << 5);
    r[mLAPF1] = r[mLAPF2] + (d << 5);
    r[mRAPF1] = r[mRAPF2] + (d << 5);
    r[vWALL] = feedback * 258;
    for (int i : {mLSAME, mRSAME, dLSAME, mLCOMB1, mLAPF1, mRAPF1, vWALL}) {
      r[i] &= 0xffff;
    }
  }

  // SPU2 addresses are in halfwords (libsd writes the table's values << 2); delays keep their
  // length in time at another output rate
  const double scale = mRate / 48000.0;
  auto addr = [&](int reg) { return (u32)std::lround(r[reg] * 4 * scale); };
  auto vol = [&](int reg) { return (s32)(s16)r[reg]; };

  mIirVol = vol(vIIR);
  if (mRate != 48000) {
    // the same damping per second: the one-pole filter's pole to the power 48000 / rate
    const double pole = 1.0 - mIirVol / 32768.0;
    const double iir = 1.0 - std::pow(std::max(pole, 0.0), 48000.0 / mRate);
    mIirVol = std::clamp<s32>((s32)std::lround(iir * 32768.0), INT16_MIN, INT16_MAX);
  }
  mWallVol = vol(vWALL);
  mApf1Vol = vol(vAPF1);
  mApf2Vol = vol(vAPF2);
  mCombVol[0] = vol(vCOMB1);
  mCombVol[1] = vol(vCOMB2);
  mCombVol[2] = vol(vCOMB3);
  mCombVol[3] = vol(vCOMB4);

  const u32 apf1 = addr(dAPF1), apf2 = addr(dAPF2);
  for (int s = 0; s < 2; s++) {
    Side& side = mSide[s];
    side.same_src = addr(s ? dRSAME : dLSAME);
    side.same_dst = addr(s ? mRSAME : mLSAME);
    side.diff_src = addr(s ? dLDIFF : dRDIFF);
    side.diff_dst = addr(s ? mRDIFF : mLDIFF);
    side.comb[0] = addr(s ? mRCOMB1 : mLCOMB1);
    side.comb[1] = addr(s ? mRCOMB2 : mLCOMB2);
    side.comb[2] = addr(s ? mRCOMB3 : mLCOMB3);
    side.comb[3] = addr(s ? mRCOMB4 : mLCOMB4);
    side.apf1_dst = addr(s ? mRAPF1 : mLAPF1);
    side.apf2_dst = addr(s ? mRAPF2 : mLAPF2);
    side.in_vol = vol(s ? vRIN : vLIN);
  }

  u32 len = (u32)std::lround(kWorkSize[mode] * 4 * scale);
  for (auto& side : mSide) {
    for (u32 a : {side.same_src, side.same_dst, side.diff_src, side.diff_dst, side.comb[0],
                  side.comb[1], side.comb[2], side.comb[3], side.apf1_dst, side.apf2_dst}) {
      len = std::max(len, a + 1);
    }
  }
  // offsets relative to the buffer address, all within [0, len)
  auto wrap = [&](s64 a) { return (u32)(((a % len) + len) % len); };
  for (auto& side : mSide) {
    side.same_prv = wrap((s64)side.same_dst - 1);
    side.diff_prv = wrap((s64)side.diff_dst - 1);
    side.apf1_src = wrap((s64)side.apf1_dst - apf1);
    side.apf2_src = wrap((s64)side.apf2_dst - apf2);
  }

  mLen = len;
  mWork.assign(2 * len, 0);
  Clear();
}

void Reverb::Clear() {
  std::fill(mWork.begin(), mWork.end(), 0);
  std::memset(mDown, 0, sizeof(mDown));
  std::memset(mUp, 0, sizeof(mUp));
  mPos = 0;
  mCycle = 0;
}

// the block's state in locals (the work area stores would otherwise reload every member)
struct Reverb::Run {
  s16* work;
  u32 len;
  u32 pos;
  u32 cycle;
  s16 (*down)[128];
  s16 (*up)[128];
  s32 iir, wall, apf1, apf2, comb[4];
  s32 depth_l, depth_r;
};

template <int R>
inline void Reverb::Step(Run& run, const Side& side, const s16Output& wet, s16Output& out) {
  const u32 fp = run.cycle & 63;
  run.down[0][fp] = run.down[0][fp | 64] = wet.left;
  run.down[1][fp] = run.down[1][fp | 64] = wet.right;

  // the work area is kept twice in a row: a read needs no wrap, a write goes to both copies
  s16* const work = run.work;
  const u32 len = run.len, pos = run.pos;
  const s16* const base = work + pos;
  auto at = [&](u32 off) -> s32 { return base[off]; };
  auto put = [&](u32 off, s32 v) {
    const u32 a = pos + off;
    work[a] = work[a >= len ? a - len : a + len] = (s16)Clamp16(v);
  };

  // the input: the side's wet signal through the half-band filter (the newest sample not yet)
  const s16* d = &run.down[R][(fp - 39) & 63];
  const s32 in = Mul(side.in_vol, Clamp16((FirEven(d) + 16384 * d[19]) >> 15));

  const s32 same_prv = at(side.same_prv);
  const s32 same = Mul(run.iir, in + Mul(run.wall, at(side.same_src)) - same_prv) + same_prv;
  const s32 diff_prv = at(side.diff_prv);
  const s32 diff = Mul(run.iir, in + Mul(run.wall, at(side.diff_src)) - diff_prv) + diff_prv;

  s32 o = Mul(run.comb[0], at(side.comb[0])) + Mul(run.comb[1], at(side.comb[1])) +
          Mul(run.comb[2], at(side.comb[2])) + Mul(run.comb[3], at(side.comb[3]));

  const s32 apf1_src = at(side.apf1_src);
  const s32 apf1 = o - Mul(run.apf1, apf1_src);
  o = apf1_src + Mul(run.apf1, apf1);
  const s32 apf2_src = at(side.apf2_src);
  const s32 apf2 = o - Mul(run.apf2, apf2_src);
  o = apf2_src + Mul(run.apf2, apf2);

  put(side.same_dst, same);
  put(side.diff_dst, diff);
  put(side.apf1_dst, apf1);
  put(side.apf2_dst, apf2);

  run.up[R][fp] = run.up[R][fp | 64] = (s16)Clamp16(o);
  run.up[R ^ 1][fp] = run.up[R ^ 1][fp | 64] = 0;

  // the output: each side's half-rate signal with zeros between through the filter (gain 2); the
  // zeros leave this side the even taps, the other side the centre one
  const u32 up_at = (fp + 1 - 39) & 63;
  const s32 mine = Clamp16((2 * FirEven(&run.up[R][up_at])) >> 15);
  const s32 other = Clamp16((32767 * run.up[R ^ 1][up_at + 19]) >> 15);
  const s32 l = R ? other : mine, r = R ? mine : other;
  out.left = (s16)Clamp16(out.left + Mul(l, run.depth_l));
  out.right = (s16)Clamp16(out.right + Mul(r, run.depth_r));

  run.cycle++;
  if (R) {
    run.pos = pos + 1 == len ? 0 : pos + 1;
  }
}

void Reverb::Process(const s16Output* wet, s16Output* out, int n) {
  if (!Active()) {
    mWasActive = false;
    return;
  }
  if (!mWasActive) {
    // the effect didn't run while its output was silent: it starts again from silence
    Clear();
    mWasActive = true;
  }

  Run run{mWork.data(), mLen, mPos, mCycle, mDown, mUp, mIirVol,
          mWallVol, mApf1Vol, mApf2Vol, {mCombVol[0], mCombVol[1], mCombVol[2], mCombVol[3]},
          mDepthL, mDepthR};
  const Side left = mSide[0], right = mSide[1];
  int i = 0;
  if (n > 0 && (run.cycle & 1)) {
    Step<1>(run, right, wet[0], out[0]);
    i = 1;
  }
  for (; i + 1 < n; i += 2) {
    Step<0>(run, left, wet[i], out[i]);
    Step<1>(run, right, wet[i + 1], out[i + 1]);
  }
  if (i < n) {
    Step<0>(run, left, wet[i], out[i]);
  }
  mPos = run.pos;
  mCycle = run.cycle;
}

}  // namespace snd
