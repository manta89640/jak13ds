// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
// (AI-assisted) Frozen copy of game/sound/common/bitfield.h at 446cce66e (before the block-wise
// ADPCM decode), the reference of test/snd_voice/voice_exact.cpp. Only the namespace (snd_ref)
// and the include paths differ.
#pragma once

namespace snd_ref {

/* Bitfield convenience class for use with unions
 * should be allowed as per common initial subsequence rule regarding unions */
template <typename sourcetype, typename type, int pos, int width>
class bitfield {
 public:
  constexpr type get() { return static_cast<type>((underlying >> pos) & mask()); };

  constexpr void set(type value) {
    underlying &= ~(mask() << pos);
    underlying |= (value & mask()) << pos;
  };

 private:
  sourcetype underlying;

  constexpr int mask() { return (1 << width) - 1; };
};
}  // namespace snd_ref
