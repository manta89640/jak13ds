/*!
 * @file CEmitter.cpp
 * The C backend. See CEmitter.h and docs/3ds-port/c_backend.md.
 */

#include "CEmitter.h"

#include <stdexcept>

#include "Env.h"
#include "IR.h"

#include "common/log/log.h"

#include "fmt/format.h"

namespace {
// tokens are resolved after memory layout: \x01 <kind> <index> \x02
constexpr char TOKEN_START = '\x01';
constexpr char TOKEN_END = '\x02';

// the C backend uses the ARM64 register conventions in the front end.
const emitter::RegisterInfo& c_reg_info() {
  return emitter::reg_info(emitter::InstructionSet::ARM64);
}

bool is_128(RegClass c) {
  return c == RegClass::INT_128 || c == RegClass::VECTOR_FLOAT;
}

const char* c_type(RegClass c) {
  switch (c) {
    case RegClass::GPR_64:
      return "u64";
    case RegClass::FLOAT:
      return "float";
    case RegClass::INT_128:
    case RegClass::VECTOR_FLOAT:
      return "v128";
    default:
      throw std::runtime_error("C backend: invalid register class");
  }
}

std::string escape_comment(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '*' || c == '/' || c == '\n' || c == TOKEN_START || c == TOKEN_END) {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string c_string_literal(const std::string& in) {
  std::string out = "\"";
  for (unsigned char c : in) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c < 32 || c > 126) {
      out += fmt::format("\\{:03o}", c);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}
}  // namespace

u64 c_backend_hash(const std::string& text) {
  u64 hash = 0xcbf29ce484222325ull;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

////////////////////////////
// CFunctionEmitter
////////////////////////////

CFunctionEmitter::CFunctionEmitter(CModuleEmitter* module, FunctionEnv* env, int f_idx)
    : m_module(module), m_env(env), m_f_idx(f_idx) {
  const auto& ri = c_reg_info();
  m_vars.resize(env->max_vars());

  for (auto& rv : env->reg_vals()) {
    auto id = rv->ireg().id;
    auto& v = m_vars.at(id);
    auto cls = rv->ireg().reg_class;
    if (v.used && v.reg_class != cls) {
      // prefer the widest class. Accesses in another class are converted.
      if (is_128(cls)) {
        v.reg_class = cls;
      }
    } else if (!v.used) {
      v.reg_class = cls;
    }
    v.used = true;
    if (rv->forced_on_stack()) {
      v.on_stack = true;
    }
  }

  for (auto& c : env->constraints()) {
    if (!c.contrain_everywhere) {
      continue;
    }
    auto& v = m_vars.at(c.ireg.id);
    const auto& r = c.desired_register;
    if (r == ri.get_process_reg()) {
      v.special = Special::PP;
    } else if (r == ri.get_st_reg()) {
      v.special = Special::ST;
    } else if (r == ri.get_offset_reg() || r == ri.get_exec_base_reg()) {
      v.special = Special::OFF;
    } else if (r == ri.get_stack_reg()) {
      v.special = Special::SP;
    }
  }
}

const CFunctionEmitter::VarInfo& CFunctionEmitter::var(const RegVal* rv) const {
  return m_vars.at(rv->ireg().id);
}

std::string CFunctionEmitter::var_name(int id) const {
  return fmt::format("r{}", id);
}

void CFunctionEmitter::error(const std::string& msg) {
  throw std::runtime_error(fmt::format("C backend error in function {}: {}", m_env->name(), msg));
}

GameVersion CFunctionEmitter::version() const {
  return m_module->version();
}

std::string CFunctionEmitter::access(const RegVal* rv, bool for_write) {
  const auto& v = var(rv);
  auto want = rv->ireg().reg_class;
  switch (v.special) {
    case Special::PP:
      return "goalc_pp";
    case Special::ST:
      if (for_write) {
        error("the st register can't be written");
      }
      return "goalc_st";
    case Special::OFF:
      if (for_write) {
        error("the off register can't be written");
      }
      return "((u64)(uintptr_t)goalc_mem)";
    case Special::SP:
      if (for_write) {
        error("the stack pointer can't be written");
      }
      return "goalc_get_sp()";
    case Special::NONE:
      break;
  }

  auto name = var_name(rv->ireg().id);
  if (v.reg_class == want || (is_128(v.reg_class) && is_128(want))) {
    return name;
  }
  if (is_128(v.reg_class)) {
    if (want == RegClass::GPR_64) {
      return name + ".du64[0]";
    }
    if (want == RegClass::FLOAT) {
      return name + ".f[0]";
    }
  }
  error(fmt::format("register {} is used as both {} and {}", rv->print(), c_type(v.reg_class),
                    c_type(want)));
}

std::string CFunctionEmitter::read(const RegVal* rv) {
  return access(rv, false);
}

std::string CFunctionEmitter::write(const RegVal* rv) {
  return access(rv, true);
}

void CFunctionEmitter::line(const std::string& text) {
  m_body.push_back("  " + text);
}

void CFunctionEmitter::move(const RegVal* dst, const RegVal* src) {
  auto dc = dst->ireg().reg_class;
  auto sc = src->ireg().reg_class;
  auto s = read(src);
  auto d = write(dst);
  if (d == s) {
    return;
  }
  std::string value;
  if (dc == sc || (is_128(dc) && is_128(sc))) {
    value = s;
  } else if (sc == RegClass::FLOAT && dc == RegClass::GPR_64) {
    value = fmt::format("gc_f2g({})", s);
  } else if (sc == RegClass::GPR_64 && dc == RegClass::FLOAT) {
    value = fmt::format("gc_g2f({})", s);
  } else if (is_128(sc) && dc == RegClass::FLOAT) {
    value = fmt::format("gc_q2f({})", s);
  } else if (sc == RegClass::FLOAT && is_128(dc)) {
    value = fmt::format("gc_f2q({})", s);
  } else if (sc == RegClass::GPR_64 && is_128(dc)) {
    value = fmt::format("gc_g2q({})", s);
  } else if (is_128(sc) && dc == RegClass::GPR_64) {
    value = fmt::format("gc_q2g({})", s);
  } else {
    error("unhandled move");
  }
  line(fmt::format("{} = {};", d, value));
}

int CFunctionEmitter::symbol(const std::string& name) {
  return m_module->symbol(name);
}

std::string CFunctionEmitter::static_addr(const emitter::StaticRecord& rec, int offset) {
  return fmt::format("(GC_SEG({}) + {} + {})", rec.seg, m_module->static_token(rec), offset);
}

std::string CFunctionEmitter::function_addr(int f_idx) {
  return fmt::format("(GC_SEG({}) + {})", m_module->function_seg.at(f_idx),
                     m_module->function_token(f_idx));
}

std::string CFunctionEmitter::stack_slot_addr(int slot) {
  if (slot < 0 || slot >= m_env->stack_slots_used_for_stack_vars()) {
    error("bad stack slot");
  }
  return fmt::format("gc_goal_ptr(&gc_stack[{}])", slot);
}

std::string CFunctionEmitter::label(int ir_idx) {
  return fmt::format("L{}", ir_idx);
}

void CFunctionEmitter::emit() {
  const auto& ri = c_reg_info();
  const auto& code = m_env->code();

  if (m_env->is_asm_func) {
    error("asm-func is not supported by the C backend, add an INSTRUCTION_SET 'c branch");
  }

  // find parameters: the IR_ValueReset at the start lists them in order (then self, for
  // behaviors). 128-bit parameters use their own argument register numbering, so the position in
  // this list is the argument index.
  std::vector<std::pair<int, int>> params;  // arg index, ireg
  int n_args = 0;
  if (!code.empty()) {
    if (auto* reset = dynamic_cast<IR_ValueReset*>(code.front().get())) {
      for (auto* rv : reset->args()) {
        if (var(rv).special != Special::NONE) {
          continue;  // self in behaviors
        }
        bool is_arg = false;
        for (auto& c : m_env->constraints()) {
          if (c.instr_idx == 0 && !c.contrain_everywhere && c.ireg.id == rv->ireg().id) {
            for (int i = 0; i < emitter::RegisterInfo::N_ARGS; i++) {
              if (c.desired_register == ri.get_gpr_arg_reg(i) ||
                  c.desired_register == ri.get_simd_arg_reg(i)) {
                is_arg = true;
              }
            }
          }
        }
        if (!is_arg) {
          error(fmt::format("could not find argument register for {}", rv->print()));
        }
        params.emplace_back(n_args, rv->ireg().id);
        n_args++;
      }
    }
  }

  // find the return register
  int return_var = -1;
  for (auto& ir : code) {
    if (auto* ret = dynamic_cast<IR_Return*>(ir.get())) {
      auto id = ret->return_reg()->ireg().id;
      if (return_var >= 0 && return_var != id) {
        error("multiple return registers");
      }
      return_var = id;
    }
  }
  if (return_var >= 0) {
    m_ret_is_128 = is_128(m_vars.at(return_var).reg_class);
  }

  // jump targets
  std::vector<bool> is_target(code.size() + 1, false);
  for (auto& ir : code) {
    if (auto* g = dynamic_cast<IR_GotoLabel*>(ir.get())) {
      is_target.at(g->dest_idx()) = true;
    } else if (auto* b = dynamic_cast<IR_ConditionalBranch*>(ir.get())) {
      is_target.at(b->label.idx) = true;
    }
  }

  // body
  for (int i = 0; i < (int)code.size(); i++) {
    if (is_target.at(i)) {
      m_body.push_back(label(i) + ":;");
    }
    code.at(i)->do_codegen_c(*this);
  }
  if (is_target.at(code.size())) {
    m_body.push_back(label(code.size()) + ":;");
  }

  // signature
  std::vector<std::string> arg_types(n_args, "u64");
  for (auto& [idx, id] : params) {
    arg_types.at(idx) = is_128(m_vars.at(id).reg_class) ? "v128" : "u64";
  }
  std::string text = fmt::format("// {}\n", escape_comment(m_env->name()));
  text += fmt::format("static {} gc_f{}(", ret_type_name(), m_f_idx);
  for (int i = 0; i < n_args; i++) {
    if (i) {
      text += ", ";
    }
    text += fmt::format("{} a{}", arg_types.at(i), i);
  }
  if (n_args == 0) {
    text += "void";
  }
  text += ") {\n";

  // locals
  for (int id = 0; id < (int)m_vars.size(); id++) {
    const auto& v = m_vars.at(id);
    if (!v.used || v.special != Special::NONE) {
      continue;
    }
    const char* init = is_128(v.reg_class) ? "{{0}}" : "0";
    text += fmt::format("  {}{} {} = {};\n", v.on_stack ? "volatile " : "", c_type(v.reg_class),
                        var_name(id), init);
  }
  if (m_env->stack_slots_used_for_stack_vars() > 0) {
    text += fmt::format("  u64 gc_stack[{}] __attribute__((aligned(16)));\n",
                        m_env->stack_slots_used_for_stack_vars());
  }
  for (auto& [idx, id] : params) {
    text += fmt::format("  {} = a{};\n", var_name(id), idx);
  }

  for (auto& l : m_body) {
    text += l;
    text += '\n';
  }

  if (return_var >= 0) {
    text += fmt::format("  return {};\n", var_name(return_var));
  } else {
    text += m_ret_is_128 ? "  { v128 z = {{0}}; return z; }\n" : "  return 0;\n";
  }
  text += "}\n\n";
  m_module->function_text[m_f_idx] = text;
}

////////////////////////////
// CModuleEmitter
////////////////////////////

CModuleEmitter::CModuleEmitter(std::string obj_name,
                               emitter::ObjectGenerator* gen,
                               GameVersion version)
    : m_obj_name(std::move(obj_name)), m_gen(gen), m_version(version) {}

void CModuleEmitter::add_function(FunctionEnv* env, int f_idx, int seg) {
  function_seg[f_idx] = seg;
  CFunctionEmitter fe(this, env, f_idx);
  try {
    fe.emit();
  } catch (std::exception& e) {
    // OPENGOAL_C_BACKEND_LENIENT=1: replace unsupported functions with a stub that breaks, so the
    // whole game can be compiled to find every problem in one pass.
    const char* lenient = std::getenv("OPENGOAL_C_BACKEND_LENIENT");
    if (!lenient || std::string(lenient) != "1") {
      throw;
    }
    lg::warn("{}", e.what());
    function_text[f_idx] = fmt::format(
        "// {} (NOT SUPPORTED BY THE C BACKEND)\nstatic u64 gc_f{}(void) {{\n  goalc_break();\n  "
        "return 0;\n}}\n\n",
        escape_comment(env->name()), f_idx);
  }
}

int CModuleEmitter::symbol(const std::string& name) {
  auto it = m_symbol_idx.find(name);
  if (it != m_symbol_idx.end()) {
    return it->second;
  }
  int idx = m_symbols.size();
  m_symbols.push_back(name);
  m_symbol_idx[name] = idx;
  return idx;
}

std::string CModuleEmitter::static_token(const emitter::StaticRecord& rec) {
  int idx = m_static_tokens.size();
  m_static_tokens.push_back(rec);
  return fmt::format("{}S{}{}", TOKEN_START, idx, TOKEN_END);
}

std::string CModuleEmitter::function_token(int f_idx) {
  int idx = m_function_tokens.size();
  m_function_tokens.push_back(f_idx);
  return fmt::format("{}F{}{}", TOKEN_START, idx, TOKEN_END);
}

std::string CModuleEmitter::finish(u64* hash_out) {
  std::string out;
  out += fmt::format("// GOAL object {} compiled to C by goalc. Do not edit.\n",
                     escape_comment(m_obj_name));
  out += "#include \"goal_c_ops.h\"\n\n";

  // symbols
  int n_syms = std::max(1, (int)m_symbols.size());
  out += fmt::format("static int32_t gc_sym_offsets[{}];\n", n_syms);
  out += "static uint32_t gc_seg_base[3];\n";
  out += fmt::format("static const char* const gc_sym_names[{}] = {{\n", n_syms);
  for (auto& s : m_symbols) {
    out += fmt::format("  {},\n", c_string_literal(s));
  }
  if (m_symbols.empty()) {
    out += "  0,\n";
  }
  out += "};\n\n";

  // functions
  for (auto& [idx, text] : function_text) {
    out += text;
  }

  int n_funcs = std::max(1, (int)function_text.size());
  out += fmt::format("static void* const gc_funcs[{}] = {{\n", n_funcs);
  for (auto& [idx, text] : function_text) {
    out += fmt::format("  (void*)gc_f{},\n", idx);
  }
  if (function_text.empty()) {
    out += "  0,\n";
  }
  out += "};\n\n";

  // the hash is computed over everything up to here (after token resolution), then embedded.
  // resolve tokens
  std::string resolved;
  resolved.reserve(out.size());
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] != TOKEN_START) {
      resolved.push_back(out[i]);
      continue;
    }
    char kind = out.at(i + 1);
    size_t end = out.find(TOKEN_END, i);
    int idx = std::stoi(out.substr(i + 2, end - i - 2));
    int value = 0;
    if (kind == 'S') {
      value = m_gen->get_static_location(m_static_tokens.at(idx));
    } else if (kind == 'F') {
      value = m_gen->get_function_location(m_gen->get_existing_function_record(
          m_function_tokens.at(idx)));
    } else {
      ASSERT(false);
    }
    resolved += std::to_string(value);
    i = end;
  }

  u64 hash = c_backend_hash(resolved);
  *hash_out = hash;

  resolved += fmt::format(
      "static const GoalCModule gc_module = {{\n"
      "  0x{:016x}ull, {}, {}, gc_funcs, {}, gc_sym_names, gc_sym_offsets, gc_seg_base\n"
      "}};\n\n",
      hash, c_string_literal(m_obj_name), function_text.size(), m_symbols.size());
  // statically linked modules are found by a generated registry (scripts/3ds/gen_c_registry.py)
  std::string cname;
  for (char c : m_obj_name) {
    cname.push_back(isalnum((unsigned char)c) ? c : '_');
  }
  cname += fmt::format("_{:08x}", (u32)c_backend_hash(m_obj_name));
  resolved += fmt::format(
      "#ifdef GOALC_STATIC\n"
      "const GoalCModule* goalc_static_{}(void) {{\n"
      "  return &gc_module;\n"
      "}}\n"
      "#else\n",
      cname);
  resolved +=
      "__attribute__((visibility(\"default\"))) const GoalCModule* goalc_module_desc(void) {\n"
      "  return &gc_module;\n"
      "}\n"
      "#endif\n";
  return resolved;
}
