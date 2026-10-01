// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC

#include "envelope.h"

#include <algorithm>
#include <array>

namespace snd {
// (AI-assisted) Run() (envelope.h) when the phase's target is reached
void ADSR::NextPhase() {
  switch (m_Phase) {
    case Phase::Attack:
      m_Phase = Phase::Decay;
      break;
    case Phase::Decay:
      m_Phase = Phase::Sustain;
      break;
    case Phase::Release:
      m_Phase = Phase::Stopped;
      break;
    default:
      break;
  }

  UpdateSettings();
}

void ADSR::UpdateSettings() {
  switch (m_Phase) {
    case Phase::Attack:
      m_Exp = m_Reg.AttackExp.get();
      m_Decrease = false;
      m_Shift = m_Reg.AttackShift.get();
      m_Step = static_cast<s8>(7 - m_Reg.AttackStep.get());
      m_Target = 0x7FFF;
      break;
    case Phase::Decay:
      m_Exp = true;
      m_Decrease = true;
      m_Shift = m_Reg.DecayShift.get();
      m_Step = -8;
      m_Target = (m_Reg.SustainLevel.get() + 1) << 11;
      break;
    case Phase::Sustain:
      m_Exp = m_Reg.SustainExp.get();
      m_Decrease = m_Reg.SustainDecr.get();
      m_Shift = m_Reg.SustainShift.get();
      m_Step = m_Decrease ? static_cast<s8>(-8 + m_Reg.SustainStep.get())
                          : static_cast<s8>(7 - m_Reg.SustainStep.get());
      m_Target = 0;  // unused for sustain
      break;
    case Phase::Release:
      m_Exp = m_Reg.ReleaseExp.get();
      m_Decrease = true;
      m_Shift = m_Reg.ReleaseShift.get();
      m_Step = -8;
      m_Target = 0;
      break;
    default:
      break;
  }
}

void ADSR::Attack() {
  m_Phase = Phase::Attack;
  m_Level = 0;
  m_Counter = 0;
  UpdateSettings();
}

void ADSR::Release() {
  m_Phase = Phase::Release;
  m_Counter = 0;
  UpdateSettings();
}

void ADSR::Stop() {
  m_Phase = Phase::Stopped;
  m_Level = 0;
}

void Volume::Run() {
  if (!m_Sweep.EnableSweep.get())
    return;

  Step();
}

void Volume::Set(u16 volume) {
  m_Sweep.bits = volume;

  if (!m_Sweep.EnableSweep.get()) {
    m_Level = static_cast<s16>(m_Sweep.bits << 1);
    return;
  }

  m_Exp = m_Sweep.SweepExp.get();
  m_Decrease = m_Sweep.SweepDecrease.get();
  m_Shift = m_Sweep.SweepShift.get();
  m_Step = m_Decrease ? static_cast<s8>(-8 + m_Sweep.SweepStep.get())
                      : static_cast<s8>(7 - m_Sweep.SweepStep.get());

  if (m_Exp && m_Decrease) {
    // if (m_Sweep.NegativePhase)
    //	Console.WriteLn("Disqualified from inv");
    m_Inv = false;
  } else {
    m_Inv = m_Sweep.NegativePhase.get();
  }

  // Console.WriteLn(Color_Red, "start sweep, e:%d d:%d sh:%d st:%d inv:%d", m_Exp, m_Decrease,
  // m_Shift, m_Step, m_Inv); Console.WriteLn(Color_Red, "Current level %08x", m_Level);
}

u16 Volume::Get() const {
  return m_Sweep.bits;
}

s16 Volume::GetCurrent() const {
  return static_cast<s16>(m_Level);
}
}  // namespace snd
