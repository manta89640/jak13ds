/*
 * (AI-assisted)
 * config.ini asm_mem = off (hardware hang bisect): plain C memcpy / memmove / memset that the
 * ctr_mem.S entry points branch to while ctr_mem_asm is 0. Only byte loads and stores, and word
 * ones when both pointers are word aligned: nothing here can raise an alignment fault.
 *
 * ctr_mem_asm starts at 0, so everything before main (libctru init, constructors) uses these;
 * ctr_platform_init sets it from config.ini (default on). Built with -ffreestanding
 * -fno-builtin -fno-tree-loop-distribute-patterns (CMakeLists.txt): gcc must not turn the loops
 * back into memcpy / memset calls.
 */

#include <stddef.h>
#include <stdint.h>

int ctr_mem_asm = 0;

void* ctr_mem_c_memcpy(void* dst, const void* src, size_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
    for (; n >= 4; n -= 4, d += 4, s += 4) {
      *(volatile uint32_t*)d = *(const volatile uint32_t*)s;
    }
  }
  for (; n; n--) {
    *(volatile uint8_t*)d++ = *s++;
  }
  return dst;
}

void* ctr_mem_c_memmove(void* dst, const void* src, size_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  if (d == s || n == 0) {
    return dst;
  }
  if ((uintptr_t)d - (uintptr_t)s >= n) {
    return ctr_mem_c_memcpy(dst, src, n); /* forward is safe */
  }
  /* dst inside [src, src + n): backward */
  d += n;
  s += n;
  if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
    for (; n >= 4; n -= 4) {
      d -= 4;
      s -= 4;
      *(volatile uint32_t*)d = *(const volatile uint32_t*)s;
    }
  }
  for (; n; n--) {
    *(volatile uint8_t*)--d = *--s;
  }
  return dst;
}

void* ctr_mem_c_memset(void* dst, int c, size_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t b = (uint8_t)c;
  for (; n && ((uintptr_t)d & 3); n--) {
    *(volatile uint8_t*)d++ = b;
  }
  const uint32_t w = b * 0x01010101u;
  for (; n >= 4; n -= 4, d += 4) {
    *(volatile uint32_t*)d = w;
  }
  for (; n; n--) {
    *(volatile uint8_t*)d++ = b;
  }
  return dst;
}
