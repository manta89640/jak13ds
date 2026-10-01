// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

// This class is only valid for sizes that are power of two
template <typename Tp, size_t Nm>
class fifo {
 public:
  Tp Pop() { return array[Mask(read++)]; }
  void Push(Tp val) { array[Mask(write++)] = val; }

  Tp& Front() { return array[Mask(read)]; }
  Tp& Back() { return array[Mask(write)]; }

  Tp Peek() { return array[Mask(read + 1)]; }
  Tp Peek(size_t offset) { return array[Mask(read + offset)]; }

  size_t Size() { return write - read; }
  bool Full() { return Size() == capacity; }
  bool Empty() { return read == write; }

  void Reset() {
    array.fill(Tp{});
    read = 0;
    write = 0;
  }

 private:
  static constexpr bool is_power_of_two(int v) { return v && ((v & (v - 1)) == 0); }
  static_assert(is_power_of_two(Nm), "FIFO size must be power of 2 for correct operation");

  size_t Mask(size_t val) { return val & (capacity - 1); }

  std::array<Tp, Nm> array = {};
  size_t capacity = Nm;
  size_t read = {};
  size_t write = {};
};

// (AI-assisted) A fifo whose contents are contiguous in memory: Data()[0 .. Size()) are the values
// in order, so the voice interpolation reads its 4 samples with plain loads (no wrapping). Append()
// moves the contents back to the start of the array when they would run past its end. At most Nm
// values may be stored.
template <typename Tp, size_t Nm>
class linear_fifo {
 public:
  Tp Pop() { return array[read++]; }
  void Push(Tp val) { *Append(1) = val; }
  // n more values at the end (n <= Nm), written through the returned pointer
  Tp* Append(size_t n) {
    if (write + n > array.size()) {
      std::copy(array.begin() + read, array.begin() + write, array.begin());
      write -= read;
      read = 0;
    }
    Tp* p = array.data() + write;
    write += n;
    return p;
  }
  Tp Peek(size_t offset) const { return array[read + offset]; }
  const Tp* Data() const { return array.data() + read; }
  void Skip(size_t n) { read += n; }
  size_t Size() const { return write - read; }
  // for loops that keep the positions in registers: Base()[ReadPos() .. WritePos()) are the
  // values (Base() stays the same, Append() may change both positions)
  const Tp* Base() const { return array.data(); }
  size_t ReadPos() const { return read; }
  size_t WritePos() const { return write; }
  void SetReadPos(size_t pos) { read = pos; }

  void Reset() {
    array.fill(Tp{});
    read = 0;
    write = 0;
  }

 private:
  std::array<Tp, 2 * Nm> array = {};
  size_t read = 0;
  size_t write = 0;
};
