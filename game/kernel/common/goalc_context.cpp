/*!
 * @file goalc_context.cpp
 * Minimal context switching for GOAL code compiled to C (see goalc_runtime.h):
 *  - goalc_ctx_save / goalc_ctx_restore: setjmp/longjmp without signal masks. Only callee-saved
 *    registers, the stack pointer and the return address are saved.
 *  - goalc_call_on_stack: call a function on another stack.
 *
 * Supported: AArch64 (AAPCS64, including Apple), ARM32 (AAPCS, ARMv6+, ARM or Thumb callers,
 * optional VFP), x86-64 System V. Other hosts do not get these symbols and fail to link if the
 * C backend is used.
 */

#include "goalc_runtime.h"

#include <cstring>

#if defined(__APPLE__)
#define GOALC_SYM(name) "_" #name
#define GOALC_FUNC_BEGIN(name) \
  ".globl " GOALC_SYM(name) "\n" \
  ".p2align 4\n" GOALC_SYM(name) ":\n"
#define GOALC_FUNC_END(name) ""
#else
#define GOALC_SYM(name) #name
#define GOALC_FUNC_BEGIN(name)                          \
  ".globl " GOALC_SYM(name) "\n"                        \
  ".type " GOALC_SYM(name) ", %function\n"              \
  ".p2align 4\n" GOALC_SYM(name) ":\n"
#define GOALC_FUNC_END(name) ".size " GOALC_SYM(name) ", .-" GOALC_SYM(name) "\n"
#endif

#if defined(__aarch64__)

// ctx layout (8-byte words):
//  0-9: x19-x28, 10: x29 (fp), 11: x30 (lr), 12: sp, 13-20: d8-d15
// x18 is the platform register and is left alone.
asm(".text\n"
    GOALC_FUNC_BEGIN(goalc_ctx_save)
    "  stp x19, x20, [x0, #0]\n"
    "  stp x21, x22, [x0, #16]\n"
    "  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n"
    "  stp x27, x28, [x0, #64]\n"
    "  stp x29, x30, [x0, #80]\n"
    "  mov x9, sp\n"
    "  str x9, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n"
    "  stp d10, d11, [x0, #120]\n"
    "  stp d12, d13, [x0, #136]\n"
    "  stp d14, d15, [x0, #152]\n"
    "  mov w0, #0\n"
    "  ret\n"
    GOALC_FUNC_END(goalc_ctx_save)

    GOALC_FUNC_BEGIN(goalc_ctx_restore)
    "  ldp x19, x20, [x0, #0]\n"
    "  ldp x21, x22, [x0, #16]\n"
    "  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n"
    "  ldp x27, x28, [x0, #64]\n"
    "  ldp x29, x30, [x0, #80]\n"
    "  ldr x9, [x0, #96]\n"
    "  mov sp, x9\n"
    "  ldp d8, d9, [x0, #104]\n"
    "  ldp d10, d11, [x0, #120]\n"
    "  ldp d12, d13, [x0, #136]\n"
    "  ldp d14, d15, [x0, #152]\n"
    "  cmp w1, #0\n"
    "  csinc w0, w1, wzr, ne\n"  // w0 = (w1 != 0) ? w1 : 1
    "  ret\n"
    GOALC_FUNC_END(goalc_ctx_restore)

    // x0 = stack_top, x1 = fn, x2 = arg
    GOALC_FUNC_BEGIN(goalc_call_on_stack)
    "  stp x29, x30, [sp, #-32]!\n"
    "  mov x29, sp\n"
    "  str x19, [sp, #16]\n"
    "  mov x19, sp\n"  // old sp, callee-saved across the call
    "  and x9, x0, #0xfffffffffffffff0\n"
    "  mov sp, x9\n"
    "  mov x9, x1\n"
    "  mov x0, x2\n"
    "  blr x9\n"
    "  mov sp, x19\n"
    "  ldr x19, [sp, #16]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  ret\n"
    GOALC_FUNC_END(goalc_call_on_stack));

uintptr_t goalc_ctx_sp(const goalc_ctx* ctx) {
  return ctx->regs[12];
}

#elif defined(__arm__)

// ctx layout (4-byte words):
//  0-7: r4-r11, 8: sp, 9: lr, 10-25: d8-d15 (only with a hardware FPU)
// The functions are ARM code; blx/bx interwork with Thumb callers (ARMv5T+).
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
#define GOALC_ARM_SAVE_VFP \
  "  add r2, r0, #40\n"    \
  "  vstmia r2, {d8-d15}\n"
#define GOALC_ARM_RESTORE_VFP \
  "  add r2, r0, #40\n"       \
  "  vldmia r2, {d8-d15}\n"
#define GOALC_ARM_FPU ".fpu vfp\n"
#else
#define GOALC_ARM_SAVE_VFP ""
#define GOALC_ARM_RESTORE_VFP ""
#define GOALC_ARM_FPU ""
#endif

asm(".text\n"
    ".syntax unified\n"
    GOALC_ARM_FPU
    ".arm\n"
    GOALC_FUNC_BEGIN(goalc_ctx_save)
    "  stmia r0, {r4-r11}\n"
    "  str sp, [r0, #32]\n"
    "  str lr, [r0, #36]\n"
    GOALC_ARM_SAVE_VFP
    "  mov r0, #0\n"
    "  bx lr\n"
    GOALC_FUNC_END(goalc_ctx_save)

    GOALC_FUNC_BEGIN(goalc_ctx_restore)
    GOALC_ARM_RESTORE_VFP
    "  ldmia r0, {r4-r11}\n"
    "  ldr sp, [r0, #32]\n"
    "  ldr lr, [r0, #36]\n"
    "  movs r0, r1\n"
    "  moveq r0, #1\n"
    "  bx lr\n"
    GOALC_FUNC_END(goalc_ctx_restore)

    // r0 = stack_top, r1 = fn, r2 = arg. Returns a u64 in r0:r1 (whatever fn returned).
    GOALC_FUNC_BEGIN(goalc_call_on_stack)
    "  push {r4, lr}\n"
    "  mov r4, sp\n"  // old sp, callee-saved across the call
    "  bic r0, r0, #15\n"
    "  mov sp, r0\n"
    "  mov r3, r1\n"
    "  mov r0, r2\n"
    "  blx r3\n"
    "  mov sp, r4\n"
    "  pop {r4, pc}\n"
    GOALC_FUNC_END(goalc_call_on_stack)
    // go back to the instruction set the compiler uses for the rest of this file
#if defined(__thumb__)
    ".thumb\n"
#endif
    );

uintptr_t goalc_ctx_sp(const goalc_ctx* ctx) {
  u32 sp;
  memcpy(&sp, (const u8*)ctx->regs + 32, 4);
  return sp;
}

#elif defined(__x86_64__) && !defined(_WIN32)

// ctx layout (8-byte words):
//  0: rbx, 1: rbp, 2-5: r12-r15, 6: rsp (after return), 7: return address
asm(".text\n"
    GOALC_FUNC_BEGIN(goalc_ctx_save)
    "  movq %rbx, 0(%rdi)\n"
    "  movq %rbp, 8(%rdi)\n"
    "  movq %r12, 16(%rdi)\n"
    "  movq %r13, 24(%rdi)\n"
    "  movq %r14, 32(%rdi)\n"
    "  movq %r15, 40(%rdi)\n"
    "  leaq 8(%rsp), %rdx\n"
    "  movq %rdx, 48(%rdi)\n"
    "  movq (%rsp), %rdx\n"
    "  movq %rdx, 56(%rdi)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    GOALC_FUNC_END(goalc_ctx_save)

    GOALC_FUNC_BEGIN(goalc_ctx_restore)
    "  movq 0(%rdi), %rbx\n"
    "  movq 8(%rdi), %rbp\n"
    "  movq 16(%rdi), %r12\n"
    "  movq 24(%rdi), %r13\n"
    "  movq 32(%rdi), %r14\n"
    "  movq 40(%rdi), %r15\n"
    "  movq 48(%rdi), %rsp\n"
    "  movl %esi, %eax\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  movl $1, %eax\n"
    "1:\n"
    "  jmpq *56(%rdi)\n"
    GOALC_FUNC_END(goalc_ctx_restore)

    // rdi = stack_top, rsi = fn, rdx = arg
    GOALC_FUNC_BEGIN(goalc_call_on_stack)
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  pushq %rbx\n"
    "  subq $8, %rsp\n"
    "  movq %rsp, %rbx\n"  // old sp, callee-saved across the call
    "  andq $-16, %rdi\n"
    "  movq %rdi, %rsp\n"
    "  movq %rdx, %rdi\n"
    "  callq *%rsi\n"
    "  movq %rbx, %rsp\n"
    "  addq $8, %rsp\n"
    "  popq %rbx\n"
    "  popq %rbp\n"
    "  ret\n"
    GOALC_FUNC_END(goalc_call_on_stack));

uintptr_t goalc_ctx_sp(const goalc_ctx* ctx) {
  return ctx->regs[6];
}

#endif
