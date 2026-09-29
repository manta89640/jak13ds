#pragma once

/*
 * (AI-assisted)
 * Minimal <sys/mman.h> for the 3DS (newlib has none). Only what the runtime uses:
 * anonymous private mappings, backed by the application heap. Address hints are ignored,
 * protections are ignored (no MMU control from userland), MAP_FIXED is not supported.
 */

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_32BIT 0x40
#define MAP_POPULATE 0x8000

#define MAP_FAILED ((void*)-1)

static inline void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
  (void)addr;
  (void)prot;
  (void)fd;
  (void)off;
  if (!(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  void* mem = memalign(0x1000, len);
  if (!mem) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  memset(mem, 0, len);  // anonymous mappings are zero-filled
  return mem;
}

static inline int munmap(void* addr, size_t len) {
  (void)len;
  free(addr);
  return 0;
}

static inline int mprotect(void* addr, size_t len, int prot) {
  (void)addr;
  (void)len;
  (void)prot;
  return 0;  // not supported: memory stays read/write
}

#ifdef __cplusplus
}
#endif
