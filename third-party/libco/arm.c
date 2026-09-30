#define LIBCO_C
#include "libco.h"
#include "settings.h"

#include <assert.h>
#include <stdlib.h>
#ifdef LIBCO_MPROTECT
  #include <unistd.h>
  #include <sys/mman.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

static thread_local unsigned long co_active_buffer[64];
static thread_local cothread_t co_active_handle = 0;
static void (*co_swap)(cothread_t, cothread_t) = 0;

/* (AI-assisted, OpenGOAL 3DS port) The switch is a real function instead of instructions in a
 * const array called as code: on the 3DS that array ends up in the read-only data segment, which
 * the hardware doesn't execute (prefetch abort at the first IOP thread; emulators don't check).
 * It also saves d8-d15, which the hard float ABI makes callee saved.
 * Context: r4-r11, sp, lr (10 words), then d8-d15 (16 words); a new context starts at lr. */
__attribute__((naked, noinline, target("arm")))
static void co_swap_function(cothread_t to, cothread_t from) {
  (void)to;
  (void)from;
  __asm__ volatile(
    "stmia r1!, {r4-r11, sp, lr}\n\t"
#ifndef __SOFTFP__
    "vstmia r1!, {d8-d15}\n\t"
#endif
    "ldmia r0!, {r4-r11, sp, lr}\n\t"
#ifndef __SOFTFP__
    "vldmia r0!, {d8-d15}\n\t"
#endif
    "bx lr\n\t");
}

static void co_init() {}

cothread_t co_active() {
  if(!co_active_handle) co_active_handle = &co_active_buffer;
  return co_active_handle;
}

cothread_t co_derive(void* memory, unsigned int size, void (*entrypoint)(void)) {
  unsigned long* handle;
  if(!co_swap) {
    co_init();
    co_swap = co_swap_function;
  }
  if(!co_active_handle) co_active_handle = &co_active_buffer;

  if(handle = (unsigned long*)memory) {
    unsigned int offset = (size & ~15);
    unsigned long* p = (unsigned long*)((unsigned char*)handle + offset);
    handle[8] = (unsigned long)p;
    handle[9] = (unsigned long)entrypoint;
  }

  return handle;
}

cothread_t co_create(unsigned int size, void (*entrypoint)(void)) {
  void* memory = malloc(size);
  if(!memory) return (cothread_t)0;
  return co_derive(memory, size, entrypoint);
}

void co_delete(cothread_t handle) {
  free(handle);
}

void co_switch(cothread_t handle) {
  cothread_t co_previous_handle = co_active_handle;
  co_swap(co_active_handle = handle, co_previous_handle);
}

int co_serializable() {
  return 1;
}

#ifdef __cplusplus
}
#endif
