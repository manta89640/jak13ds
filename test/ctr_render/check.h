#pragma once

// (AI-assisted) Keep regression checks active in Release builds too.
#include <stdexcept>

inline void check(bool ok, const char* message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}
