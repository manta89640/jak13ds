#pragma once

/*!
 * @file CEmitter.h
 * The C backend: turns the IR of the functions in an object file into a C module.
 * See docs/3ds-port/c_backend.md.
 *
 * Each IR instruction prints C statements through CFunctionEmitter (IR::do_codegen_c).
 * IR registers become C locals (register allocation is not used). The GOAL registers with a fixed
 * meaning (pp, st, off, sp) become runtime globals. Addresses that depend on the memory layout of
 * the object are printed as tokens and resolved in CModuleEmitter::finish, after layout.
 */

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/versions/versions.h"

#include "goalc/emitter/ObjectGenerator.h"
#include "goalc/emitter/Register.h"
#include "goalc/regalloc/allocator_interface.h"

class FunctionEnv;
class RegVal;
class IR;

class CModuleEmitter;

class CFunctionEmitter {
 public:
  CFunctionEmitter(CModuleEmitter* module, FunctionEnv* env, int f_idx);
  void emit();

  // used by IR::do_codegen_c
  //! C expression to read a register value (in the register class of rv)
  std::string read(const RegVal* rv);
  //! C lvalue to write a register value (in the register class of rv). Throws for read-only regs.
  std::string write(const RegVal* rv);
  //! C statement(s) for "dst = src" with the conversions of regset_common
  void move(const RegVal* dst, const RegVal* src);
  //! add a line to the function body
  void line(const std::string& text);
  //! symbol index in the module
  int symbol(const std::string& name);
  //! C expression for the GOAL address of static data (plus offset)
  std::string static_addr(const emitter::StaticRecord& rec, int offset);
  //! C expression for the GOAL address of a function in this object
  std::string function_addr(int f_idx);
  //! C expression for a stack variable slot's GOAL address
  std::string stack_slot_addr(int slot);
  //! C label name for an IR index
  std::string label(int ir_idx);
  GameVersion version() const;
  [[noreturn]] void error(const std::string& msg);

  std::string ret_type_name() const { return m_ret_is_128 ? "v128" : "u64"; }

 private:
  enum class Special { NONE, PP, ST, OFF, SP };
  struct VarInfo {
    bool used = false;
    RegClass reg_class = RegClass::GPR_64;
    Special special = Special::NONE;
    bool on_stack = false;
  };
  const VarInfo& var(const RegVal* rv) const;
  std::string var_name(int id) const;
  std::string access(const RegVal* rv, bool for_write);

  CModuleEmitter* m_module = nullptr;
  FunctionEnv* m_env = nullptr;
  int m_f_idx = -1;
  std::vector<VarInfo> m_vars;
  std::vector<std::string> m_body;
  bool m_ret_is_128 = false;
};

class CModuleEmitter {
 public:
  CModuleEmitter(std::string obj_name, emitter::ObjectGenerator* gen, GameVersion version);
  //! generate C for one function. Can be called before layout.
  void add_function(FunctionEnv* env, int f_idx, int seg);
  //! resolve layout tokens and build the complete source. Call after generate_data_v3.
  std::string finish(u64* hash_out);

  int symbol(const std::string& name);
  std::string static_token(const emitter::StaticRecord& rec);
  std::string function_token(int f_idx);
  GameVersion version() const { return m_version; }
  emitter::ObjectGenerator* gen() { return m_gen; }

  //! function definitions (C text), by function index
  std::map<int, std::string> function_text;
  //! seg of each function, by function index
  std::map<int, int> function_seg;

 private:
  std::string m_obj_name;
  emitter::ObjectGenerator* m_gen = nullptr;
  GameVersion m_version;
  std::vector<std::string> m_symbols;
  std::unordered_map<std::string, int> m_symbol_idx;
  std::vector<emitter::StaticRecord> m_static_tokens;
  std::vector<int> m_function_tokens;
};

//! 64-bit FNV-1a, used for module hashes.
u64 c_backend_hash(const std::string& text);
