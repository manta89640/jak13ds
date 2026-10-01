// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

#include "bitfield.h"

#include "common/common_types.h"

namespace snd {
union ADSRReg {
  u32 bits;

  bitfield<u32, u16, 16, 16> hi;
  bitfield<u32, u16, 0, 16> lo;

  bitfield<u32, bool, 31, 1> SustainExp;
  bitfield<u32, bool, 30, 1> SustainDecr;
  bitfield<u32, u8, 29, 1> Unused;
  bitfield<u32, u8, 24, 5> SustainShift;
  bitfield<u32, u8, 22, 2> SustainStep;
  bitfield<u32, bool, 21, 1> ReleaseExp;
  bitfield<u32, u8, 16, 5> ReleaseShift;

  bitfield<u32, bool, 15, 1> AttackExp;
  bitfield<u32, u8, 10, 5> AttackShift;
  bitfield<u32, u8, 8, 2> AttackStep;
  bitfield<u32, u8, 4, 4> DecayShift;
  bitfield<u32, u8, 0, 4> SustainLevel;
};

union VolReg {
  u16 bits;

  bitfield<u16, bool, 15, 1> EnableSweep;
  bitfield<u16, bool, 14, 1> SweepExp;
  bitfield<u16, bool, 13, 1> SweepDecrease;
  bitfield<u16, bool, 12, 1> NegativePhase;
  bitfield<u16, u8, 2, 5> SweepShift;
  bitfield<u16, u8, 0, 2> SweepStep;
};

class Envelope {
 public:
  void Step();
  // (AI-assisted) what Step() adds to the counter at the current settings and level
  [[nodiscard]] u32 CounterStep() const;

 protected:
  u8 m_Shift{0};
  s8 m_Step{0};
  bool m_Inv{false};
  bool m_Exp{false};
  bool m_Decrease{false};

  u32 m_Counter{0};
  s32 m_Level{0};
};

class ADSR : Envelope {
 public:
  enum class Phase {
    Attack,
    Decay,
    Sustain,
    Release,
    Stopped,
  };

  void Run();
  // (AI-assisted) How many of the next Run() calls change nothing but the counter (no level or
  // phase change): they can be done at once with SkipQuiet. 0: the next Run() may change more.
  [[nodiscard]] int QuietSamples() const;
  void SkipQuiet(int n) { m_Counter += CounterStep() * (u32)n; }
  void Attack();
  void Release();
  void Stop();
  [[nodiscard]] s16 Level() const;
  void SetLevel(s16 value) { m_Level = value; }
  void UpdateSettings();
  ADSRReg m_Reg{0};
  [[nodiscard]] Phase GetPhase() const { return m_Phase; }

  void Reset() {
    m_Phase = Phase::Stopped;
    m_Target = 0;
    m_Counter = 0;
    m_Level = 0;
    m_Exp = false;
    m_Decrease = false;
    m_Inv = false;
    m_Step = 0;
    m_Shift = 0;
  }

 private:
  void NextPhase();

  Phase m_Phase{Phase::Stopped};
  s32 m_Target{0};
};

class Volume : Envelope {
 public:
  void Run();
  // (AI-assisted) Run() does nothing without a sweep
  [[nodiscard]] bool Sweeping() const { return (m_Sweep.bits & 0x8000) != 0; }  // EnableSweep
  void Set(u16 volume);
  [[nodiscard]] u16 Get() const;
  [[nodiscard]] s16 GetCurrent() const;

  void Reset() {
    m_Sweep.bits = 0;

    m_Counter = 0;
    m_Level = 0;
    m_Exp = false;
    m_Decrease = false;
    m_Inv = false;
    m_Step = 0;
    m_Shift = 0;
  }

 private:
  VolReg m_Sweep{0};
};

struct VolumePair {
  Volume left{};
  Volume right{};

  void Run() {
    left.Run();
    right.Run();
  }

  void Reset() {
    left.Reset();
    right.Reset();
  }
};
// (AI-assisted) The per tick functions are here, inline: the 3DS mixer (Voice::RunBlock) runs them
// for every voice at every envelope change.

inline void Envelope::Step() {
  // arbitrary number of bits, this is probably incorrect for the
  // "reserved" and infinite duration values
  // test hw or copy mednafen instead?
  u32 cStep = 0x800000;

  s32 shift = m_Shift - 11;
  if (shift > 0)
    cStep >>= shift;

  s16 step = static_cast<s16>(m_Step << std::max(0, 11 - m_Shift));

  if (m_Exp) {
    if (!m_Decrease && m_Level > 0x6000)
      cStep >>= 2;

    if (m_Decrease)
      step = static_cast<s16>((step * m_Level) >> 15);
  }

  m_Counter += cStep;

  if (m_Counter >= 0x800000) {
    m_Counter = 0;
    m_Level = std::clamp<s32>(m_Level + step, 0, INT16_MAX);
  }
}

inline u32 Envelope::CounterStep() const {
  // as Step() computes it
  u32 cStep = 0x800000;
  s32 shift = m_Shift - 11;
  if (shift > 0)
    cStep >>= shift;
  if (m_Exp && !m_Decrease && m_Level > 0x6000)
    cStep >>= 2;
  return cStep;
}

inline int ADSR::QuietSamples() const {
  if (m_Phase == Phase::Stopped) {
    return 0;
  }
  // Run()'s phase check after its Step(): with the level unchanged its result is the current one
  if (m_Phase != Phase::Sustain &&
      ((!m_Decrease && m_Level >= m_Target) || (m_Decrease && m_Level <= m_Target))) {
    return 0;
  }
  // Step() calls until the counter reaches 0x800000 (always below it between calls): the last one
  // changes the level, the ones before only add to the counter. The step is 0x800000 shifted
  // right, a power of two: the division is a shift (no divide instruction on the 3DS's ARM11).
  const u32 cStep = CounterStep();
  const u32 calls = (0x800000u - m_Counter + cStep - 1) >> std::countr_zero(cStep);
  return calls > 0x7fffffffu ? 0x7fffffff : (int)(calls - 1);
}

inline void ADSR::Run() {
  // Let's not waste time calculating silent voices
  if (m_Phase == Phase::Stopped)
    return;

  Step();

  if (m_Phase == Phase::Sustain)
    return;

  if ((!m_Decrease && m_Level >= m_Target) || (m_Decrease && m_Level <= m_Target)) {
    NextPhase();
  }
}

inline s16 ADSR::Level() const {
  return static_cast<u16>(m_Level);
}
}  // namespace snd
