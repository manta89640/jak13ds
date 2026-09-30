/*!
 * @file IR_C.cpp
 * C backend code generation for each IR. The semantics match the x86-64 backend in IR.cpp.
 */

#include "CEmitter.h"
#include "Env.h"
#include "IR.h"

#include "common/symbols.h"

#include "fmt/format.h"

namespace {
bool is_128(RegClass c) {
  return c == RegClass::INT_128 || c == RegClass::VECTOR_FLOAT;
}

std::string hex(u64 x) {
  return fmt::format("0x{:x}ull", x);
}

std::string sym_value_load(CFunctionEmitter& e, const std::string& name, bool sext) {
  return fmt::format("gc_ld_{}32(GC_SYM({}))", sext ? "s" : "u", e.symbol(name));
}

//! write a u64 value into a register of any class
void set_from_gpr(CFunctionEmitter& e, const RegVal* dst, const std::string& value) {
  auto c = dst->ireg().reg_class;
  if (c == RegClass::GPR_64) {
    e.line(fmt::format("{} = {};", e.write(dst), value));
  } else if (is_128(c)) {
    e.line(fmt::format("{} = gc_g2q({});", e.write(dst), value));
  } else {
    e.line(fmt::format("{} = gc_g2f({});", e.write(dst), value));
  }
}
}  // namespace

void IR_Return::do_codegen_c(CFunctionEmitter& e) {
  e.move(m_return_reg, m_value);
}

void IR_LoadConstant64::do_codegen_c(CFunctionEmitter& e) {
  set_from_gpr(e, m_dest, hex(m_value));
}

void IR_LoadSymbolPointer::do_codegen_c(CFunctionEmitter& e) {
  std::string value;
  if (m_name == "#f") {
    value = "gc_stl";
  } else if (m_name == "#t") {
    value = fmt::format("(gc_stl + {})", true_symbol_offset(e.version()));
  } else if (m_name == "_empty_") {
    value = fmt::format("(gc_stl + {})", empty_pair_offset_from_s7(e.version()));
  } else {
    value = fmt::format("GC_SYM({})", e.symbol(m_name));
  }
  set_from_gpr(e, m_dest, value);
}

void IR_SetSymbolValue::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("gc_st_u32(GC_SYM({}), {});", e.symbol(m_dest->name()), e.read(m_src)));
}

void IR_GetSymbolValue::do_codegen_c(CFunctionEmitter& e) {
  set_from_gpr(e, m_dest, sym_value_load(e, m_src->name(), m_sext));
}

void IR_RegSet::do_codegen_c(CFunctionEmitter& e) {
  e.move(m_dest, m_src);
}

void IR_FunctionCall::do_codegen_c(CFunctionEmitter& e) {
  std::string sig;
  std::string args;
  for (size_t i = 0; i < m_args.size(); i++) {
    if (i) {
      sig += ", ";
      args += ", ";
    }
    bool wide = is_128(m_args[i]->ireg().reg_class);
    sig += wide ? "v128" : "u64";
    args += e.read(m_args[i]);
  }
  if (m_args.empty()) {
    sig = "void";
  }
  bool ret_wide = is_128(m_ret->ireg().reg_class);
  e.line(fmt::format("{} = (({} (*)({}))goalc_fn({}))({});", e.write(m_ret),
                     ret_wide ? "v128" : "u64", sig, e.read(m_func), args));
}

void IR_RegValAddr::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = gc_goal_ptr((const void*)&{});", e.write(m_dest), e.read(m_src)));
}

void IR_StaticVarAddr::do_codegen_c(CFunctionEmitter& e) {
  set_from_gpr(e, m_dest, e.static_addr(m_src->rec, m_src->get_addr_offset()));
}

void IR_StaticVarLoad::do_codegen_c(CFunctionEmitter& e) {
  auto addr = e.static_addr(m_src->rec, 0);
  auto c = m_dest->ireg().reg_class;
  if (c == RegClass::FLOAT) {
    e.line(fmt::format("{} = gc_ld_f32({});", e.write(m_dest), addr));
  } else if (c == RegClass::VECTOR_FLOAT) {
    e.line(fmt::format("{} = gc_ld_v128({});", e.write(m_dest), addr));
  } else {
    e.error("unsupported static var load");
  }
}

void IR_FunctionAddr::do_codegen_c(CFunctionEmitter& e) {
  set_from_gpr(e, m_dest, e.function_addr(m_src->idx_in_file));
}

void IR_IntegerMath::do_codegen_c(CFunctionEmitter& e) {
  auto d = e.write(m_dest);
  auto dv = e.read(m_dest);
  auto a = [&]() { return e.read(m_arg); };
  switch (m_kind) {
    case IntegerMathKind::ADD_64:
      e.line(fmt::format("{} = {} + {};", d, dv, a()));
      break;
    case IntegerMathKind::SUB_64:
      e.line(fmt::format("{} = {} - {};", d, dv, a()));
      break;
    case IntegerMathKind::AND_64:
      e.line(fmt::format("{} = {} & {};", d, dv, a()));
      break;
    case IntegerMathKind::OR_64:
      e.line(fmt::format("{} = {} | {};", d, dv, a()));
      break;
    case IntegerMathKind::XOR_64:
      e.line(fmt::format("{} = {} ^ {};", d, dv, a()));
      break;
    case IntegerMathKind::NOT_64:
      e.line(fmt::format("{} = ~{};", d, dv));
      break;
    case IntegerMathKind::SHLV_64:
      e.line(fmt::format("{} = gc_shl({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::SHRV_64:
      e.line(fmt::format("{} = gc_shr({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::SARV_64:
      e.line(fmt::format("{} = gc_sar({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::SHL_64:
      e.line(fmt::format("{} = gc_shl({}, {});", d, dv, (int)m_shift_amount));
      break;
    case IntegerMathKind::SHR_64:
      e.line(fmt::format("{} = gc_shr({}, {});", d, dv, (int)m_shift_amount));
      break;
    case IntegerMathKind::SAR_64:
      e.line(fmt::format("{} = gc_sar({}, {});", d, dv, (int)m_shift_amount));
      break;
    case IntegerMathKind::IMUL_32:
      e.line(fmt::format("{} = gc_imul32({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::IMUL_64:
      e.line(fmt::format("{} = gc_imul64({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::IDIV_32:
      e.line(fmt::format("{} = gc_idiv32({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::UDIV_32:
      e.line(fmt::format("{} = gc_udiv32({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::IMOD_32:
      e.line(fmt::format("{} = gc_imod32({}, {});", d, dv, a()));
      break;
    case IntegerMathKind::UMOD_32:
      e.line(fmt::format("{} = gc_umod32({}, {});", d, dv, a()));
      break;
    default:
      e.error("unknown integer math kind");
  }
}

void IR_FloatMath::do_codegen_c(CFunctionEmitter& e) {
  auto d = e.write(m_dest);
  auto dv = e.read(m_dest);
  auto a = e.read(m_arg);
  switch (m_kind) {
    case FloatMathKind::DIV_SS:
      e.line(fmt::format("{} = {} / {};", d, dv, a));
      break;
    case FloatMathKind::MUL_SS:
      e.line(fmt::format("{} = {} * {};", d, dv, a));
      break;
    case FloatMathKind::ADD_SS:
      e.line(fmt::format("{} = {} + {};", d, dv, a));
      break;
    case FloatMathKind::SUB_SS:
      e.line(fmt::format("{} = {} - {};", d, dv, a));
      break;
    case FloatMathKind::MIN_SS:
      e.line(fmt::format("{} = gc_fmin({}, {});", d, dv, a));
      break;
    case FloatMathKind::MAX_SS:
      e.line(fmt::format("{} = gc_fmax({}, {});", d, dv, a));
      break;
    case FloatMathKind::SQRT_SS:
      e.line(fmt::format("{} = sqrtf({});", d, a));
      break;
    default:
      e.error("unknown float math kind");
  }
}

void IR_GotoLabel::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("goto {};", e.label(m_dest->idx)));
}

void IR_ConditionalBranch::do_codegen_c(CFunctionEmitter& e) {
  const char* op = nullptr;
  switch (condition.kind) {
    case ConditionKind::EQUAL:
      op = "==";
      break;
    case ConditionKind::NOT_EQUAL:
      op = "!=";
      break;
    case ConditionKind::LEQ:
      op = "<=";
      break;
    case ConditionKind::GEQ:
      op = ">=";
      break;
    case ConditionKind::LT:
      op = "<";
      break;
    case ConditionKind::GT:
      op = ">";
      break;
    default:
      e.error("invalid condition");
  }
  auto a = e.read(condition.a);
  auto b = e.read(condition.b);
  std::string cond;
  if (condition.is_float) {
    cond = fmt::format("{} {} {}", a, op, b);
  } else if (condition.is_signed) {
    cond = fmt::format("(s64){} {} (s64){}", a, op, b);
  } else {
    cond = fmt::format("{} {} {}", a, op, b);
  }
  e.line(fmt::format("if ({}) goto {};", cond, e.label(label.idx)));
}

void IR_Null::do_codegen_c(CFunctionEmitter&) {}

void IR_ValueReset::do_codegen_c(CFunctionEmitter&) {}

void IR_FloatToInt::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = gc_f2i({});", e.write(m_dest), e.read(m_src)));
}

void IR_IntToFloat::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = gc_i2f({});", e.write(m_dest), e.read(m_src)));
}

void IR_GetStackAddr::do_codegen_c(CFunctionEmitter& e) {
  set_from_gpr(e, m_dest, e.stack_slot_addr(m_slot));
}

void IR_Nop::do_codegen_c(CFunctionEmitter&) {}

void IR_LoadConstOffset::do_codegen_c(CFunctionEmitter& e) {
  auto addr = fmt::format("{} + {}", e.read(m_base), m_offset);
  auto c = m_dest->ireg().reg_class;
  auto d = e.write(m_dest);
  if (c == RegClass::GPR_64) {
    const char* sign = m_info.sign_extend ? "s" : "u";
    switch (m_info.size) {
      case 1:
      case 2:
      case 4:
        e.line(fmt::format("{} = gc_ld_{}{}({});", d, sign, m_info.size * 8, addr));
        break;
      case 8:
        e.line(fmt::format("{} = gc_ld_u64({});", d, addr));
        break;
      default:
        e.error("bad load size");
    }
  } else if (c == RegClass::FLOAT && m_info.size == 4 && !m_info.sign_extend &&
             m_info.reg == RegClass::FLOAT) {
    e.line(fmt::format("{} = gc_ld_f32({});", d, addr));
  } else if (is_128(c) && m_info.size == 16 && !m_info.sign_extend) {
    e.line(fmt::format("{} = gc_ld_v128({});", d, addr));
  } else {
    e.error("unsupported load");
  }
}

void IR_StoreConstOffset::do_codegen_c(CFunctionEmitter& e) {
  auto addr = fmt::format("{} + {}", e.read(m_base), m_offset);
  auto c = m_value->ireg().reg_class;
  auto v = e.read(m_value);
  if (c == RegClass::GPR_64) {
    switch (m_size) {
      case 1:
      case 2:
      case 4:
      case 8:
        e.line(fmt::format("gc_st_u{}({}, {});", m_size * 8, addr, v));
        break;
      default:
        e.error("bad store size");
    }
  } else if (c == RegClass::FLOAT && m_size == 4) {
    e.line(fmt::format("gc_st_f32({}, {});", addr, v));
  } else if (is_128(c) && m_size == 16) {
    e.line(fmt::format("gc_st_v128({}, {});", addr, v));
  } else {
    e.error("unsupported store");
  }
}

void IR_AsmRet::do_codegen_c(CFunctionEmitter& e) {
  e.error(".ret is not supported by the C backend");
}

void IR_AsmPush::do_codegen_c(CFunctionEmitter& e) {
  e.error(".push is not supported by the C backend");
}

void IR_AsmPop::do_codegen_c(CFunctionEmitter& e) {
  e.error(".pop is not supported by the C backend");
}

void IR_AsmSub::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = {} - {};", e.write(m_dst), e.read(m_dst), e.read(m_src)));
}

void IR_AsmAdd::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = {} + {};", e.write(m_dst), e.read(m_dst), e.read(m_src)));
}

void IR_AsmBreak::do_codegen_c(CFunctionEmitter& e) {
  e.line("goalc_break();");
}

void IR_AsmFNop::do_codegen_c(CFunctionEmitter&) {}

void IR_AsmFWait::do_codegen_c(CFunctionEmitter&) {}

void IR_GetSymbolValueAsm::do_codegen_c(CFunctionEmitter& e) {
  set_from_gpr(e, m_dest, sym_value_load(e, m_sym_name, m_sext));
}

void IR_JumpReg::do_codegen_c(CFunctionEmitter& e) {
  e.error(".jr is not supported by the C backend");
}

void IR_RegSetAsm::do_codegen_c(CFunctionEmitter& e) {
  e.move(m_dst, m_src);
}

void IR_VFMath3Asm::do_codegen_c(CFunctionEmitter& e) {
  const char* f = nullptr;
  switch (m_kind) {
    case Kind::XOR:
      f = "gc_vf_xor";
      break;
    case Kind::SUB:
      f = "gc_vf_sub";
      break;
    case Kind::ADD:
      f = "gc_vf_add";
      break;
    case Kind::MUL:
      f = "gc_vf_mul";
      break;
    case Kind::MAX:
      f = "gc_vf_max";
      break;
    case Kind::MIN:
      f = "gc_vf_min";
      break;
    case Kind::DIV:
      f = "gc_vf_div";
      break;
    default:
      e.error("unknown vf math");
  }
  e.line(fmt::format("{} = {}({}, {});", e.write(m_dst), f, e.read(m_src1), e.read(m_src2)));
}

void IR_Int128Math3Asm::do_codegen_c(CFunctionEmitter& e) {
  const char* f = nullptr;
  switch (m_kind) {
    case Kind::PEXTUB:
      f = "gc_pextub";
      break;
    case Kind::PEXTUH:
      f = "gc_pextuh";
      break;
    case Kind::PEXTUW:
      f = "gc_pextuw";
      break;
    case Kind::PEXTLB:
      f = "gc_pextlb";
      break;
    case Kind::PEXTLH:
      f = "gc_pextlh";
      break;
    case Kind::PEXTLW:
      f = "gc_pextlw";
      break;
    case Kind::PCPYUD:
      f = "gc_pcpyud";
      break;
    case Kind::PCPYLD:
      f = "gc_pcpyld";
      break;
    case Kind::PSUBW:
      f = "gc_psubw";
      break;
    case Kind::PCEQB:
      f = "gc_pceqb";
      break;
    case Kind::PCEQH:
      f = "gc_pceqh";
      break;
    case Kind::PCEQW:
      f = "gc_pceqw";
      break;
    case Kind::PCGTB:
      f = "gc_pcgtb";
      break;
    case Kind::PCGTH:
      f = "gc_pcgth";
      break;
    case Kind::PCGTW:
      f = "gc_pcgtw";
      break;
    case Kind::POR:
      f = "gc_por";
      break;
    case Kind::PXOR:
      f = "gc_pxor";
      break;
    case Kind::PAND:
      f = "gc_pand";
      break;
    case Kind::PACKUSWB:
      f = "gc_packuswb";
      break;
    case Kind::PADDB:
      f = "gc_paddb";
      break;
    default:
      e.error("unknown int128 math");
  }
  e.line(fmt::format("{} = {}({}, {});", e.write(m_dst), f, e.read(m_src1), e.read(m_src2)));
}

void IR_Int128Math2Asm::do_codegen_c(CFunctionEmitter& e) {
  const char* f = nullptr;
  switch (m_kind) {
    case Kind::PW_SLL:
      f = "gc_pw_sll";
      break;
    case Kind::PW_SRL:
      f = "gc_pw_srl";
      break;
    case Kind::PH_SLL:
      f = "gc_ph_sll";
      break;
    case Kind::PH_SRL:
      f = "gc_ph_srl";
      break;
    case Kind::PW_SRA:
      f = "gc_pw_sra";
      break;
    case Kind::VPSRLDQ:
      f = "gc_psrldq";
      break;
    case Kind::VPSLLDQ:
      f = "gc_pslldq";
      break;
    case Kind::VPSHUFLW:
      f = "gc_pshuflw";
      break;
    case Kind::VPSHUFHW:
      f = "gc_pshufhw";
      break;
    default:
      e.error("unknown int128 math");
  }
  if (!m_imm.has_value() || *m_imm < 0 || *m_imm > 255) {
    e.error("bad immediate");
  }
  e.line(fmt::format("{} = {}({}, {});", e.write(m_dst), f, e.read(m_src), *m_imm));
}

void IR_VFMath2Asm::do_codegen_c(CFunctionEmitter& e) {
  const char* f = m_kind == Kind::ITOF ? "gc_vf_itof" : "gc_vf_ftoi";
  e.line(fmt::format("{} = {}({});", e.write(m_dst), f, e.read(m_src)));
}

void IR_BlendVF::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = gc_vf_blend({}, {}, {});", e.write(m_dst), e.read(m_src1),
                     e.read(m_src2), (int)m_mask));
}

void IR_SplatVF::do_codegen_c(CFunctionEmitter& e) {
  int lane = (int)m_element;
  if (lane < 0 || lane > 3) {
    e.error("bad splat element");
  }
  e.line(fmt::format("{} = gc_vf_splat({}, {});", e.write(m_dst), e.read(m_src), lane));
}

void IR_SwizzleVF::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = gc_vf_swizzle({}, {});", e.write(m_dst), e.read(m_src),
                     (int)m_controlBytes));
}

void IR_SqrtVF::do_codegen_c(CFunctionEmitter& e) {
  e.line(fmt::format("{} = gc_vf_sqrt({});", e.write(m_dst), e.read(m_src)));
}
