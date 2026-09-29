/*!
 * @file CodeGenerator.h
 * Generate object files from a FileEnv using an emitter::ObjectGenerator.
 * Populates a DebugInfo.
 * Currently owns the logic for emitting the function prologues.
 */

#pragma once

#include "Env.h"

#include "common/versions/versions.h"

#include "goalc/emitter/ObjectGenerator.h"

class DebugInfo;
class TypeSystem;

class CodeGenerator {
 public:
  CodeGenerator(FileEnv* env,
                DebugInfo* debug_info,
                GameVersion version,
                emitter::InstructionSet instruction_set,
                bool c_backend = false);
  std::vector<u8> run(const TypeSystem* ts);
  //! C backend output (valid after run() when c_backend is set)
  const std::string& c_source() const { return m_c_source; }
  u64 c_hash() const { return m_c_hash; }
  emitter::ObjectGeneratorStats get_obj_stats() const { return m_gen.get_stats(); }

 private:
  void do_function(FunctionEnv* env, int f_idx);
  void do_goal_function_x86(FunctionEnv* env, int f_idx);
  void do_goal_function_arm64(FunctionEnv* env, int f_idx);
  void do_asm_function(FunctionEnv* env, int f_idx, bool allow_saved_regs);
  std::vector<u8> run_c(const TypeSystem* ts);
  emitter::ObjectGenerator m_gen;
  FileEnv* m_fe = nullptr;
  DebugInfo* m_debug_info = nullptr;
  bool m_c_backend = false;
  std::string m_c_source;
  u64 m_c_hash = 0;
};
