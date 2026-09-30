// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once

#include <array>
#include <cstddef>

// This class is only valid for sizes that are power of two
template <typename Tp, size_t Nm>
class fifo {
 public:
  Tp Pop() { return array[Mask(read++)]; }
  void Push(Tp val) {
    // (AI-assisted) mirrored: the entries after any read position are contiguous (Window)
    array[Mask(write)] = val;
    array[Mask(write) + capacity] = val;
    write++;
  }
  // drop n entries (n Pops without reading them)
  void Skip(size_t n) { read += n; }

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

  // (AI-assisted) for loops that keep the read position in a local: the entries from read
  // position r are Window(r)[0 .. capacity - 1], without wrapping around
  size_t ReadPos() const { return read; }
  size_t WritePos() const { return write; }
  void SetReadPos(size_t r) { read = r; }
  const Tp* Window(size_t r) const { return &array[Mask(r)]; }

 private:
  static constexpr bool is_power_of_two(int v) { return v && ((v & (v - 1)) == 0); }
  static_assert(is_power_of_two(Nm), "FIFO size must be power of 2 for correct operation");

  static constexpr size_t Mask(size_t val) { return val & (capacity - 1); }

  static constexpr size_t capacity = Nm;
  std::array<Tp, 2 * Nm> array = {};
  size_t read = {};
  size_t write = {};
};
