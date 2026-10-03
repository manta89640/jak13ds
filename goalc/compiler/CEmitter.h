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
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/versions/versions.h"

#include "goalc/emitter/ObjectGenerator.h"
#include "goalc/emitter/Register.h"
#include "goalc/regalloc/allocator_interface.h"

class FunctionEnv;
class RegVal;
class IR;
class TypeSystem;
class TypeSpec;

/*!
 * (AI-assisted) How a GPR argument is passed to a compiled GOAL function (ABI version 3, see
 * docs/3ds-port/c_backend.md). Every argument passes its low 32 bits as a u32; WIDE arguments also
 * pass their upper 32 bits (as the bits of a float, in the VFP/SIMD argument registers).
 */
enum class CArgClass {
  WIDE,   //! any 64-bit value: the receiver uses lo | hi << 32
  PTR,    //! the type is a GOAL address (structure, pointer, pair...): the value is zero-extended
  FLOAT,  //! float: the value is sign-extended (like a float moved to a GPR)
};
//! the class of an argument of type t (unknown types are WIDE)
CArgClass c_arg_class(const TypeSystem* ts, const TypeSpec& t);
//! bit i set if argument i of a function type is WIDE (all set for `function` and _varargs_)
u32 c_abi_wide_mask(const TypeSystem* ts, const TypeSpec& function_type);

class CModuleEmitter;

class CFunctionEmitter {
 public:
  CFunctionEmitter(CModuleEmitter* module, FunctionEnv* env, int f_idx);
  void emit();

  // used by IR::do_codegen_c
  //! C expression to read a register value (in the register class of rv)
  std::string read(const RegVal* rv);
  //! C lvalue to write a register value (in the register class of rv). Throws for read-only regs.
  //! The statement must be added with line() before the next write().
  std::string write(const RegVal* rv);
  //! C statement(s) for "dst = src" with the conversions of regset_common
  void move(const RegVal* dst, const RegVal* src);
  //! add a line to the function body
  void line(const std::string& text);
  //! symbol index in the module
  int symbol(const std::string& name);
  //! C expression for the GOAL address of static data (plus offset)
  std::string static_addr(const emitter::StaticRecord& rec, int offset);
  //! C expressions (base, offset) for a load/store of static data (plus offset)
  std::pair<std::string, std::string> static_access(const emitter::StaticRecord& rec, int offset);
  //! after a call: the callee may have changed the process register
  void after_call();
  //! C expression for the GOAL address of a function in this object
  std::string function_addr(int f_idx);
  //! C expression for a stack variable slot's GOAL address
  std::string stack_slot_addr(int slot);
  //! C label name for an IR index
  std::string label(int ir_idx);
  GameVersion version() const;
  const TypeSystem* ts() const;
  [[noreturn]] void error(const std::string& msg);

  std::string ret_type_name() const { return m_ret_is_128 ? "v128" : "u64"; }

  // (AI-assisted) what a register is known to hold, for guarded direct calls (see GC_GCALL in
  // goal_c_ops.h). Only a hint: forgotten on every other write and at every label.
  //! rv now holds the value of the global symbol `name`
  void set_known_symbol(const RegVal* rv, const std::string& name);
  //! rv now holds the address of function f_idx of this object
  void set_known_function(const RegVal* rv, int f_idx);
  //! the symbol whose value rv holds, or nullptr
  const std::string* known_symbol(const RegVal* rv) const;
  //! the function of this object whose address rv holds, or -1
  int known_function(const RegVal* rv) const;
  CModuleEmitter* module() { return m_module; }

 private:
  enum class Special { NONE, PP, ST, OFF, SP };
  struct VarInfo {
    bool used = false;
    RegClass reg_class = RegClass::GPR_64;
    Special special = Special::NONE;
    bool on_stack = false;
    //! used in more than one register class: declared as a gc_m128 union
    bool mixed = false;
  };
  const VarInfo& var(const RegVal* rv) const;
  std::string var_name(int id) const;
  std::string access(const RegVal* rv, bool for_write);
  std::string var_expr(int id, RegClass want);
  std::string seg_addr(int seg, const std::string& offset);
  //! base expression of our own segment (a local set in the prologue)
  std::string own_seg_base();

  CModuleEmitter* m_module = nullptr;
  FunctionEnv* m_env = nullptr;
  int m_f_idx = -1;
  std::vector<VarInfo> m_vars;
  std::vector<std::string> m_body;
  bool m_ret_is_128 = false;
  //! the process register is read or written: it is kept in the local gc_pp, see emit()
  bool m_uses_pp = false;
  //! the statement being built writes the process register
  bool m_pp_written = false;
  bool m_uses_own_seg = false;
  struct Known {
    std::string symbol;
    int function = -1;
  };
  std::unordered_map<int, Known> m_known;
};

class CModuleEmitter {
 public:
  CModuleEmitter(std::string obj_name,
                 emitter::ObjectGenerator* gen,
                 GameVersion version,
                 const TypeSystem* ts);
  //! generate C for one function. Can be called before layout.
  void add_function(FunctionEnv* env, int f_idx, int seg);
  //! resolve layout tokens and build the complete source. Call after generate_data_v3.
  std::string finish(u64* hash_out);

  int symbol(const std::string& name);
  std::string static_token(const emitter::StaticRecord& rec);
  std::string function_token(int f_idx);
  GameVersion version() const { return m_version; }
  emitter::ObjectGenerator* gen() { return m_gen; }
  const TypeSystem* ts() const { return m_ts; }

  //! function definitions (C text), by function index
  std::map<int, std::string> function_text;
  //! seg of each function, by function index
  std::map<int, int> function_seg;

  // (AI-assisted) guarded direct calls (GC_GCALL in goal_c_ops.h)
  //! symbols whose calls are never guarded (functions provided by the runtime), or nullptr
  const std::unordered_set<std::string>* no_guard_symbols = nullptr;
  //! index of the call target macro (GC_T<index>) for calls of the function in symbol `name`, or
  //! -1 when such calls aren't guarded
  int guard_target(const std::string& name);
  //! this object stores function f_idx in the symbol `name`
  void note_define(const std::string& name, int f_idx) { m_defines[name] = f_idx; }

 private:
  std::string m_obj_name;
  emitter::ObjectGenerator* m_gen = nullptr;
  GameVersion m_version;
  const TypeSystem* m_ts = nullptr;
  std::vector<std::string> m_symbols;
  std::unordered_map<std::string, int> m_symbol_idx;
  std::vector<emitter::StaticRecord> m_static_tokens;
  std::vector<int> m_function_tokens;
  std::vector<std::string> m_guard_symbols;
  std::unordered_map<std::string, int> m_guard_idx;
  std::map<std::string, int> m_defines;
};

//! 64-bit FNV-1a, used for module hashes.
u64 c_backend_hash(const std::string& text);
