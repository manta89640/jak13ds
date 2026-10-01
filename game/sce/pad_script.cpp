/*!
 * @file pad_script.cpp
 * (AI-assisted)
 * Scripted controller input, see pad_script.h.
 */

#include "pad_script.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "fmt/format.h"

namespace pad_script {
namespace {

struct Entry {
  u64 frame = 0;
  PadState state;
  std::vector<std::string> prints;
  std::vector<std::string> logs;
  std::vector<std::string> positions;
  std::vector<std::string> continues;
  std::string wait_sym, wait_state;
  bool exit = false;
  bool crash = false;
  bool save = false;
};

struct Script {
  bool loaded = false;
  bool enabled = false;
  std::vector<Entry> entries;
  size_t next = 0;
  u64 frame = 0;
  u64 real_frame = 0;
  PadState current;
  // waiting for a state
  bool waiting = false;
  std::string wait_sym, wait_state;
  u64 wait_start = 0;
};

Script g_script;
PrintSymbolHook g_print_hook = nullptr;
StateNameHook g_state_hook = nullptr;
PositionHook g_position_hook = nullptr;
ExitHook g_exit_hook = nullptr;
ContinueHook g_continue_hook = nullptr;
SaveHook g_save_hook = nullptr;

// PadData::ButtonIndex
int button_index(const std::string& name) {
  static const char* kNames[16] = {"select", "l3",  "r3",       "start",  "up",    "right",
                                   "down",   "left", "l2",      "r2",     "l1",    "r1",
                                   "triangle", "circle", "cross", "square"};
  if (name == "x") {
    return 14;
  }
  for (int i = 0; i < 16; i++) {
    if (name == kNames[i]) {
      return i;
    }
  }
  return -1;
}

void load() {
  g_script.loaded = true;
  const char* path = getenv("OPENGOAL_PAD_SCRIPT");
  if (!path || !path[0]) {
    return;
  }
  std::ifstream f(path);
  if (!f.good()) {
    lg::error("[pad script] can't open {}", path);
    return;
  }
  std::string line;
  int line_no = 0;
  while (std::getline(f, line)) {
    line_no++;
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::istringstream ss(line);
    Entry e;
    if (!(ss >> e.frame)) {
      continue;  // blank line
    }
    std::string item;
    while (ss >> item) {
      if (item == "-") {
        continue;
      } else if (item == "print") {
        std::string sym;
        ss >> sym;
        e.prints.push_back(sym);
      } else if (item == "log") {
        std::string rest;
        std::getline(ss, rest);
        e.logs.push_back(rest);
      } else if (item == "pos") {
        std::string sym;
        ss >> sym;
        e.positions.push_back(sym);
      } else if (item == "continue") {
        std::string name;
        ss >> name;
        e.continues.push_back(name);
      } else if (item == "wait") {
        ss >> e.wait_sym >> e.wait_state;
      } else if (item == "exit") {
        e.exit = true;
      } else if (item == "save") {
        e.save = true;
      } else if (item == "crash") {
        e.crash = true;
      } else if (item.size() > 3 && item[2] == '=') {
        int v = atoi(item.c_str() + 3);
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        std::string axis = item.substr(0, 2);
        if (axis == "lx") {
          e.state.lx = (u8)v;
        } else if (axis == "ly") {
          e.state.ly = (u8)v;
        } else if (axis == "rx") {
          e.state.rx = (u8)v;
        } else if (axis == "ry") {
          e.state.ry = (u8)v;
        } else {
          lg::error("[pad script] line {}: unknown axis {}", line_no, item);
        }
      } else {
        int b = button_index(item);
        if (b < 0) {
          lg::error("[pad script] line {}: unknown item {}", line_no, item);
        } else {
          e.state.buttons |= (u16)(1 << b);
        }
      }
    }
    g_script.entries.push_back(e);
  }
  g_script.enabled = true;
  lg::info("[pad script] {} entries from {}", g_script.entries.size(), path);
}

}  // namespace

bool active() {
  if (!g_script.loaded) {
    load();
  }
  return g_script.enabled;
}

PadState next_frame() {
  if (!active()) {
    return PadState();
  }
  u64 real = g_script.real_frame++;
  if (g_script.waiting) {
    std::string state = g_state_hook ? g_state_hook(g_script.wait_sym.c_str()) : "";
    if (state == g_script.wait_state) {
      lg::info("[pad script] frame {}: {} is in {} after {} frames", g_script.frame,
               g_script.wait_sym, g_script.wait_state, real - g_script.wait_start);
      g_script.waiting = false;
    } else if (real - g_script.wait_start > 30000) {
      lg::error("[pad script] frame {}: gave up waiting for {} to be in {} (it is in '{}')",
                g_script.frame, g_script.wait_sym, g_script.wait_state, state);
      g_script.waiting = false;
    } else {
      return PadState();
    }
  }
  u64 frame = g_script.frame++;
  while (g_script.next < g_script.entries.size() &&
         g_script.entries[g_script.next].frame <= frame) {
    const Entry& e = g_script.entries[g_script.next++];
    g_script.current = e.state;
    for (auto& l : e.logs) {
      lg::info("[pad script] frame {}:{}", frame, l);
    }
    for (auto& p : e.prints) {
      lg::info("[pad script] frame {}: print {}", frame, p);
      if (g_print_hook) {
        g_print_hook(p.c_str());
      }
    }
    for (auto& p : e.positions) {
      float xyz[3];
      if (g_position_hook && g_position_hook(p.c_str(), xyz)) {
        lg::info("[pad script] frame {}: {} at ({:.2f}, {:.2f}, {:.2f}) m, state {}", frame, p,
                 xyz[0], xyz[1], xyz[2], g_state_hook ? g_state_hook(p.c_str()) : "?");
      } else {
        lg::info("[pad script] frame {}: {} has no position", frame, p);
      }
    }
    for (auto& name : e.continues) {
      lg::info("[pad script] frame {}: continue {}", frame, name);
      if (g_continue_hook) {
        g_continue_hook(name.c_str());
      }
    }
    if (e.save) {
      lg::info("[pad script] frame {}: save", frame);
      if (g_save_hook) {
        g_save_hook();
      }
    }
    if (!e.wait_sym.empty()) {
      lg::info("[pad script] frame {}: waiting for {} to be in {}", frame, e.wait_sym,
               e.wait_state);
      g_script.waiting = true;
      g_script.wait_sym = e.wait_sym;
      g_script.wait_state = e.wait_state;
      g_script.wait_start = real;
      break;
    }
    if (e.crash) {
      ASSERT_MSG(false, fmt::format("pad script crash test at frame {}", frame));
    }
    if (e.exit) {
      lg::info("[pad script] frame {}: exit", frame);
      if (g_exit_hook) {
        g_exit_hook();
      }
    }
  }
  return g_script.current;
}

void set_print_symbol_hook(PrintSymbolHook hook) {
  g_print_hook = hook;
}

void set_state_name_hook(StateNameHook hook) {
  g_state_hook = hook;
}

void set_position_hook(PositionHook hook) {
  g_position_hook = hook;
}

void set_save_hook(SaveHook hook) {
  g_save_hook = hook;
}

void set_continue_hook(ContinueHook hook) {
  g_continue_hook = hook;
}

void set_exit_hook(ExitHook hook) {
  g_exit_hook = hook;
}

}  // namespace pad_script
