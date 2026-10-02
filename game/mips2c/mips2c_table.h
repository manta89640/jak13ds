#pragma once

#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/util/Assert.h"

#include "game/kernel/common/Ptr.h"

namespace Mips2C {

struct NativeImpl;

class LinkedFunctionTable {
 public:
  //! native: optional native C++ version of the function, used instead of exec (mips2c_native.h)
  void reg(const std::string& name,
           u64 (*exec)(void*),
           u32 goal_stack_size,
           const NativeImpl* native = nullptr);
  u32 get(const std::string& name);
  //! C mode (AI-assisted): get, and make the stub read the upper half of the arguments in
  //! goalc_wide_mask only (the wide arguments of the function's GOAL type, see goal_c_abi.h)
  u32 get(const std::string& name, u32 goalc_wide_mask);

 private:
  struct Func {
    u64 (*c_func)(void*);
    Ptr<u8> goal_trampoline;
    u32 stack_size = 0;
    const NativeImpl* native = nullptr;
  };
  //! C mode: the function id GOAL calls for a function
  u32 goalc_id(const std::string& name, const Func& f, u32 wide_mask);
  std::unordered_map<std::string, Func> m_executes;
};

extern PerGameVersion<std::unordered_map<std::string, std::vector<void (*)()>>>
    gMips2CLinkCallbacks;
extern LinkedFunctionTable gLinkedFunctionTable;

struct Rng {
  Rng() { init(); }
  float R = 0.;
  std::mt19937 extra_random_generator;

  u32 R_u32() {
    u32 result;
    memcpy(&result, &R, 4);
    return result;
  }

  float from23_bits(float in) {
    u32 val;
    memcpy(&val, &in, 4);
    val = 0x3F800000 | (0x0007FFFFF & val);
    memcpy(&in, &val, 4);
    return in;
  }

  float from23_bits(u32 val) {
    val = 0x3F800000 | (0x0007FFFFF & val);
    float out;
    memcpy(&out, &val, 4);
    return out;
  }

  void init(float rinit = 1.418091058731079f) { R = from23_bits(rinit); }

  void advance() {
    u32 r32 = R_u32();
    u32 x = 1 & (r32 >> 4);
    u32 y = 1 & (r32 >> 22);
    r32 <<= 1;
    r32 = r32 ^ x ^ y;
    // we add a bit of extra randomness here. They XOR the random number generator with an
    // uninitialized register, and in our port this corresponds to some random value on the stack.
    // If we get unlucky this can end up being the current value in r32, and the random generator
    // will get stuck outputting 1 for a while.
    r32 ^= extra_random_generator();
    R = from23_bits(r32);
  }

  void rxor(u32 in) { R = from23_bits(in ^ R_u32()); }
};

extern Rng gRng;
}  // namespace Mips2C