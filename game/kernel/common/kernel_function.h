#pragma once

/*!
 * @file kernel_function.h
 * A C++ function exposed to GOAL (make_function_from_c and friends), with its signature.
 *
 * In C mode (docs/3ds-port/c_backend.md), GOAL calls every function as
 *   u64 f(u64 a0, ..., u64 a7)
 * That only works directly for functions that take and return 64-bit integers. Others (u32/s32
 * parameters on ARM32, floats on any hard-float ABI, 32-bit returns) are called through an adapter
 * generated from the signature here, which converts the arguments like the native GOAL->C
 * trampolines do:
 *   - integer parameters: truncated to their type
 *   - float parameters: the low 32 bits are the float's bits (GOAL passes floats as bit patterns)
 *   - integer returns narrower than 64 bits: zero-extended (like a 32-bit return in eax/w0)
 *   - float returns: the float's bits, zero-extended
 *   - void returns: 0
 */

#include <cstring>
#include <type_traits>

#include "common/common_types.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/goalc_runtime.h"

namespace kernel_function_detail {

template <typename T>
struct is_ptr_type : std::false_type {};
template <typename T>
struct is_ptr_type<Ptr<T>> : std::true_type {};

template <typename A>
A from_goal(u64 v) {
  if constexpr (std::is_same_v<A, float>) {
    u32 bits = (u32)v;
    float f;
    memcpy(&f, &bits, 4);
    return f;
  } else if constexpr (std::is_same_v<A, double>) {
    double d;
    memcpy(&d, &v, 8);
    return d;
  } else if constexpr (std::is_integral_v<A> || std::is_enum_v<A>) {
    return static_cast<A>(v);
  } else if constexpr (is_ptr_type<A>::value) {
    return A((u32)v);
  } else if constexpr (std::is_pointer_v<A>) {
    // the native trampolines pass the register as it is (a GOAL address, or for stack argument
    // functions, a host pointer)
    return reinterpret_cast<A>((uintptr_t)v);
  } else {
    static_assert(sizeof(A) == 0, "unsupported parameter type for a function exposed to GOAL");
  }
}

template <typename R>
u64 to_goal(R r) {
  if constexpr (std::is_same_v<R, float>) {
    u32 bits;
    memcpy(&bits, &r, 4);
    return bits;
  } else if constexpr (std::is_same_v<R, double>) {
    u64 bits;
    memcpy(&bits, &r, 8);
    return bits;
  } else if constexpr (std::is_same_v<R, bool>) {
    return r ? 1 : 0;
  } else if constexpr (std::is_integral_v<R> || std::is_enum_v<R>) {
    // zero-extend, like a 32-bit return value in eax/w0 read as rax/x0.
    using U = std::make_unsigned_t<std::conditional_t<std::is_enum_v<R>, u64, R>>;
    if constexpr (std::is_enum_v<R>) {
      return (u64)r;
    } else {
      return (u64)(U)r;
    }
  } else if constexpr (is_ptr_type<R>::value) {
    return r.offset;
  } else if constexpr (std::is_pointer_v<R>) {
    // like the native trampolines, the value is returned as it is
    return (u64)(uintptr_t)r;
  } else {
    static_assert(sizeof(R) == 0, "unsupported return type for a function exposed to GOAL");
  }
}

template <typename R, typename... A, size_t... I>
u64 call_typed(R (*f)(A...), const u64* args, std::index_sequence<I...>) {
  if constexpr (std::is_void_v<R>) {
    f(from_goal<A>(args[I])...);
    return 0;
  } else {
    return to_goal<R>(f(from_goal<A>(args[I])...));
  }
}

//! extra: bit 0 set = arg 3 is the process pointer (arg3_is_pp)
template <typename R, typename... A>
u64 typed_adapter(void* fn, u64 extra, u64* args) {
  static_assert(sizeof...(A) <= 8, "functions exposed to GOAL have at most 8 arguments");
  if (extra & 1) {
    args[3] = goalc_pp;
  }
  return call_typed((R(*)(A...))fn, args, std::index_sequence_for<A...>{});
}

//! Stack argument functions: a pointer to the 8 arguments.
template <typename R, typename P>
u64 stack_args_adapter(void* fn, u64, u64* args) {
  auto f = (R(*)(P))fn;
  if constexpr (std::is_void_v<R>) {
    f((P)args);
    return 0;
  } else {
    return to_goal<R>(f((P)args));
  }
}

template <typename T>
constexpr bool is_u64_like() {
  return std::is_same_v<T, u64> || std::is_same_v<T, s64> ||
         std::is_same_v<T, unsigned long long> || std::is_same_v<T, long long>;
}

}  // namespace kernel_function_detail

/*!
 * A host function exposed to GOAL, with an adapter for C mode when it can't be called as
 * goalc_fn8. Implicitly constructed from any function pointer.
 */
struct KernelFunction {
  void* ptr = nullptr;
  //! adapter used in C mode, or null if ptr can be called as goalc_fn8 directly.
  goalc_adapter adapter = nullptr;
  //! adapter for the same function when it's exported as a stack-argument function
  goalc_adapter stack_adapter = nullptr;

  template <typename R, typename... A>
  KernelFunction(R (*f)(A...)) : ptr((void*)f) {  // NOLINT(google-explicit-constructor)
    using namespace kernel_function_detail;
    constexpr bool direct =
        (std::is_void_v<R> || is_u64_like<R>()) && (is_u64_like<A>() && ...);
    if constexpr (!direct) {
      adapter = &typed_adapter<R, A...>;
    }
    if constexpr (sizeof...(A) == 1 && (std::is_pointer_v<A> && ...)) {
      stack_adapter = &stack_args_adapter<R, A...>;
    }
  }

  //! for code that still takes an untyped function (other games)
  operator void*() const { return ptr; }  // NOLINT(google-explicit-constructor)
};
