/*
 * (AI-assisted)
 * memcpy / memmove / memset (and the __aeabi_ entry points) tuned for the ARM11 MPCore.
 *
 * devkitARM's newlib for armv6k uses the generic C versions: no LDM/STM, no PLD, and a byte loop
 * whenever src or dst is not word aligned. These replace them for the whole of gk.elf. This file
 * is linked into gk as an object (not an archive member), so its definitions always win over
 * libc.a; the matching libc.a members are then never pulled in.
 *
 * Cycle costs below are for L1 hits, from the MPCore TRM (DDI 0360F, 15.12): the load/store
 * datapath is 64 bits wide, so an 8-register LDM or STM takes 4 memory cycles.
 *   aligned copy:   PLD + LDM x8 + SUBS + STM x8 + BHS per 32 B, about 10 cycles (0.3 c/B)
 *                   (newlib: 16 B per 13 single LDR/STR instructions, about 0.85 c/B)
 *   misaligned src: LDM x8 + 16 shift/merge ALU ops + STM x8 per 32 B, about 22 cycles (0.7 c/B)
 *                   (newlib: a byte loop, about 4-5 c/B)
 *   memset:         STM x8 + SUBS + BHS per 32 B, about 6 cycles (0.2 c/B)
 * Copies under 64 B (vectors, matrices) take a short path: LDM/STM 16 B at a time, then the tail
 * by flags (no loop) when src and dst are word aligned.
 *
 * Needs no ARMv6 unaligned access: misaligned sources are read as aligned words and merged with
 * shifts. Words are only read when they hold at least one source byte, so nothing is read past
 * the page of the last byte. Test: platform/3ds/tests/memtest (compares against a byte loop).
 */

    .syntax unified
    .arm
    .section .text.og3ds_mem, "ax", %progbits

#define PLD_AHEAD 96  /* three cache lines */

    .global memcpy
    .global __aeabi_memcpy
    .global __aeabi_memcpy4
    .global __aeabi_memcpy8
    .type memcpy, %function
    .type __aeabi_memcpy, %function
    .type __aeabi_memcpy4, %function
    .type __aeabi_memcpy8, %function
    .global memmove
    .global __aeabi_memmove
    .global __aeabi_memmove4
    .global __aeabi_memmove8
    .type memmove, %function
    .type __aeabi_memmove, %function
    .type __aeabi_memmove4, %function
    .type __aeabi_memmove8, %function
    .global memset
    .global __aeabi_memset
    .global __aeabi_memset4
    .global __aeabi_memset8
    .global __aeabi_memclr
    .global __aeabi_memclr4
    .global __aeabi_memclr8
    .type memset, %function
    .type __aeabi_memset, %function
    .type __aeabi_memset4, %function
    .type __aeabi_memset8, %function
    .type __aeabi_memclr, %function
    .type __aeabi_memclr4, %function
    .type __aeabi_memclr8, %function

/* ---------------------------------------------------------------------------------------------
 * memcpy(r0 = dst, r1 = src, r2 = n). r0 is returned unchanged.
 * ------------------------------------------------------------------------------------------- */
    .p2align 5
memcpy:
__aeabi_memcpy:
__aeabi_memcpy4:
__aeabi_memcpy8:
.Lmemcpy:
    orr     ip, r0, r1
    cmp     r2, #64
    mov     r3, r0
    bhs     .Lcpy_big
    tst     ip, #3
    bne     .Lcpy_small_unal

    /* n < 64, both word aligned: 16 B per LDM/STM, then 8/4/2/1 by flags */
.Lcpy_small_al:
    push    {r4, r5, r6}
    subs    r2, r2, #16
    blo     2f
1:  ldmia   r1!, {r4, r5, r6, ip}
    subs    r2, r2, #16
    stmia   r3!, {r4, r5, r6, ip}
    bhs     1b
2:  lsls    ip, r2, #29             /* C = 8, N = 4 */
    ldmiacs r1!, {r4, r5}
    ldrmi   r6, [r1], #4
    stmiacs r3!, {r4, r5}
    strmi   r6, [r3], #4
    lsls    r2, r2, #31             /* C = 2, N = 1 */
    ldrhcs  r4, [r1], #2
    ldrbmi  r5, [r1]
    strhcs  r4, [r3], #2
    strbmi  r5, [r3]
    pop     {r4, r5, r6}
    bx      lr

.Lcpy_small_unal:
    cmp     r2, #16
    bhs     .Lcpy_big
    /* n < 16 and not word aligned: bytes */
    cmp     r2, #0
    bxeq    lr
1:  ldrb    ip, [r1], #1
    subs    r2, r2, #1
    strb    ip, [r3], #1
    bne     1b
    bx      lr

/* n >= 16. Saves r0 (the return value) and uses it as the running dst. */
.Lcpy_big:
    push    {r0, r4-r9, lr}
    /* align dst to 4 */
    ands    ip, r0, #3
    beq     1f
    rsb     ip, ip, #4
    sub     r2, r2, ip
    lsls    ip, ip, #31             /* N = 1 byte, C = 2 bytes */
    ldrbmi  r3, [r1], #1
    ldrbcs  r4, [r1], #1
    ldrbcs  r5, [r1], #1
    strbmi  r3, [r0], #1
    strbcs  r4, [r0], #1
    strbcs  r5, [r0], #1
1:  ands    ip, r1, #3
    bne     .Lcpy_merge

    /* src and dst word aligned, n >= 13. Align dst to 32 B when the copy is big. */
    cmp     r2, #64
    blo     1f
    rsb     ip, r0, #0
    ands    ip, ip, #28
    beq     1f
    sub     r2, r2, ip
    lsls    ip, ip, #28             /* C = 16, N = 8 */
    ldmiacs r1!, {r3-r6}
    stmiacs r0!, {r3-r6}
    ldmiami r1!, {r3, r4}
    stmiami r0!, {r3, r4}
    lsls    ip, ip, #2              /* C = 4 */
    ldrcs   r3, [r1], #4
    strcs   r3, [r0], #4
1:  subs    r2, r2, #32
    blo     3f
2:  pld     [r1, #PLD_AHEAD]
    ldmia   r1!, {r3-r9, ip}
    subs    r2, r2, #32
    stmia   r0!, {r3-r9, ip}
    bhs     2b
3:  /* r2 = remaining - 32; its low 5 bits are the remaining byte count */
    lsls    ip, r2, #28             /* C = 16, N = 8 */
    ldmiacs r1!, {r3-r6}
    stmiacs r0!, {r3-r6}
    ldmiami r1!, {r3, r4}
    stmiami r0!, {r3, r4}
    lsls    ip, r2, #30             /* C = 4, N = 2 */
    ldrcs   r3, [r1], #4
    strcs   r3, [r0], #4
    ldrhmi  r3, [r1], #2
    strhmi  r3, [r0], #2
    tst     r2, #1
    ldrbne  r3, [r1]
    strbne  r3, [r0]
    pop     {r0, r4-r9, pc}

/* dst word aligned, src not, n >= 13. Read aligned words and merge pairs with shifts:
 * out = (w0 >> pull) | (w1 << push), pull = 8 * (src & 3), push = 32 - pull. */
.Lcpy_merge:
    bic     r1, r1, #3
    ldr     lr, [r1], #4
    cmp     ip, #2
    beq     .Lcpy_merge2
    bhi     .Lcpy_merge3

    .macro  CPY_MERGE pull, push, back
    cmp     r2, #64
    blo     5f
    /* align dst to 32 B, one word at a time */
    rsb     ip, r0, #0
    ands    ip, ip, #28
    beq     2f
    sub     r2, r2, ip
1:  mov     r3, lr, lsr #\pull
    ldr     lr, [r1], #4
    orr     r3, r3, lr, lsl #\push
    subs    ip, ip, #4
    str     r3, [r0], #4
    bne     1b
2:  sub     r2, r2, #32
3:  pld     [r1, #PLD_AHEAD]
    mov     r3, lr, lsr #\pull
    ldmia   r1!, {r4-r9, ip, lr}
    orr     r3, r3, r4, lsl #\push
    mov     r4, r4, lsr #\pull
    orr     r4, r4, r5, lsl #\push
    mov     r5, r5, lsr #\pull
    orr     r5, r5, r6, lsl #\push
    mov     r6, r6, lsr #\pull
    orr     r6, r6, r7, lsl #\push
    mov     r7, r7, lsr #\pull
    orr     r7, r7, r8, lsl #\push
    mov     r8, r8, lsr #\pull
    orr     r8, r8, r9, lsl #\push
    mov     r9, r9, lsr #\pull
    orr     r9, r9, ip, lsl #\push
    mov     ip, ip, lsr #\pull
    orr     ip, ip, lr, lsl #\push
    subs    r2, r2, #32
    stmia   r0!, {r3-r9, ip}
    bhs     3b
    add     r2, r2, #32
5:  /* whole words left (r2 < 64 bytes) */
    subs    r2, r2, #4
    blo     7f
6:  mov     r3, lr, lsr #\pull
    ldr     lr, [r1], #4
    orr     r3, r3, lr, lsl #\push
    subs    r2, r2, #4
    str     r3, [r0], #4
    bhs     6b
7:  sub     r1, r1, #\back          /* back to the real src position */
    b       .Lcpy_tail_bytes
    .endm

    CPY_MERGE 8, 24, 3
.Lcpy_merge2:
    CPY_MERGE 16, 16, 2
.Lcpy_merge3:
    CPY_MERGE 24, 8, 1

/* the last 0-3 bytes: r2's low 2 bits */
.Lcpy_tail_bytes:
    lsls    r2, r2, #31             /* N = 1 byte, C = 2 bytes */
    ldrbcs  r3, [r1], #1
    ldrbcs  r4, [r1], #1
    ldrbmi  r5, [r1]
    strbcs  r3, [r0], #1
    strbcs  r4, [r0], #1
    strbmi  r5, [r0]
    pop     {r0, r4-r9, pc}
    .size memcpy, . - memcpy
    .size __aeabi_memcpy, . - __aeabi_memcpy
    .size __aeabi_memcpy4, . - __aeabi_memcpy4
    .size __aeabi_memcpy8, . - __aeabi_memcpy8

/* ---------------------------------------------------------------------------------------------
 * memmove(r0 = dst, r1 = src, r2 = n). Forward (memcpy) unless dst lies inside [src, src + n).
 * memcpy is safe for dst < src: every batch of loads comes before its stores, and the stores
 * stay below the bytes not loaded yet. The backward copy mirrors that from the top down.
 * ------------------------------------------------------------------------------------------- */
    .p2align 5
memmove:
__aeabi_memmove:
__aeabi_memmove4:
__aeabi_memmove8:
    subs    ip, r0, r1
    bxeq    lr
    cmp     ip, r2
    bhs     .Lmemcpy
    /* backward: r1 / r3 = src / dst end */
    add     r1, r1, r2
    add     r3, r0, r2
    cmp     r2, #16
    bhs     1f
2:  ldrb    ip, [r1, #-1]!
    subs    r2, r2, #1
    strb    ip, [r3, #-1]!
    bne     2b
    bx      lr

1:  push    {r4-r10, lr}
    /* align the dst end to 4 */
    ands    ip, r3, #3
    beq     1f
    sub     r2, r2, ip
    lsls    ip, ip, #31             /* N = 1 byte, C = 2 bytes */
    ldrbmi  r4, [r1, #-1]!
    ldrbcs  r5, [r1, #-1]!
    ldrbcs  r6, [r1, #-1]!
    strbmi  r4, [r3, #-1]!
    strbcs  r5, [r3, #-1]!
    strbcs  r6, [r3, #-1]!
1:  ands    ip, r1, #3
    bne     .Lmove_merge

    /* both word aligned, 32 B per LDMDB/STMDB */
    subs    r2, r2, #32
    blo     3f
2:  ldmdb   r1!, {r4-r10, ip}
    subs    r2, r2, #32
    stmdb   r3!, {r4-r10, ip}
    bhs     2b
3:  lsls    ip, r2, #28             /* C = 16, N = 8 */
    ldmdbcs r1!, {r4-r7}
    stmdbcs r3!, {r4-r7}
    ldmdbmi r1!, {r4, r5}
    stmdbmi r3!, {r4, r5}
    lsls    ip, r2, #30             /* C = 4, N = 2 */
    ldrcs   r4, [r1, #-4]!
    strcs   r4, [r3, #-4]!
    ldrhmi  r4, [r1, #-2]!
    strhmi  r4, [r3, #-2]!
    tst     r2, #1
    ldrbne  r4, [r1, #-1]
    strbne  r4, [r3, #-1]
    pop     {r4-r10, pc}

/* src end not word aligned: one merged word at a time (backward overlap is rare and cold).
 * out = (hi << push) | (lo >> pull), pull = 8 * (src end & 3). */
.Lmove_merge:
    bic     r1, r1, #3
    ldr     lr, [r1]
    cmp     ip, #2
    beq     .Lmove_merge2
    bhi     .Lmove_merge3

    .macro  MOVE_MERGE pull, push, fwd
    subs    r2, r2, #4
    blo     2f
1:  mov     r4, lr, lsl #\push
    ldr     lr, [r1, #-4]!
    orr     r4, r4, lr, lsr #\pull
    subs    r2, r2, #4
    str     r4, [r3, #-4]!
    bhs     1b
2:  add     r1, r1, #\fwd           /* back to the real src position */
    b       .Lmove_tail_bytes
    .endm

    MOVE_MERGE 8, 24, 1
.Lmove_merge2:
    MOVE_MERGE 16, 16, 2
.Lmove_merge3:
    MOVE_MERGE 24, 8, 3

.Lmove_tail_bytes:
    lsls    r2, r2, #31             /* N = 1 byte, C = 2 bytes */
    ldrbcs  r4, [r1, #-1]!
    ldrbcs  r5, [r1, #-1]!
    ldrbmi  r6, [r1, #-1]
    strbcs  r4, [r3, #-1]!
    strbcs  r5, [r3, #-1]!
    strbmi  r6, [r3, #-1]
    pop     {r4-r10, pc}
    .size memmove, . - memmove
    .size __aeabi_memmove, . - __aeabi_memmove
    .size __aeabi_memmove4, . - __aeabi_memmove4
    .size __aeabi_memmove8, . - __aeabi_memmove8

/* ---------------------------------------------------------------------------------------------
 * memset(r0 = dst, r1 = c, r2 = n); __aeabi_memset(dst, n, c); __aeabi_memclr(dst, n).
 * ------------------------------------------------------------------------------------------- */
    .p2align 5
__aeabi_memclr:
__aeabi_memclr4:
__aeabi_memclr8:
    mov     r2, r1
    mov     r1, #0
    b       .Lfill
__aeabi_memset:
__aeabi_memset4:
__aeabi_memset8:
    and     r3, r2, #255
    mov     r2, r1
    orr     r1, r3, r3, lsl #8
    orr     r1, r1, r1, lsl #16
    b       .Lfill
memset:
    and     r1, r1, #255
    orr     r1, r1, r1, lsl #8
    orr     r1, r1, r1, lsl #16
.Lfill:                             /* r1 = the byte repeated 4 times */
    mov     r3, r0
    cmp     r2, #4
    blo     .Lfill_lt4
    /* align dst to 4 */
    ands    ip, r3, #3
    beq     1f
    rsb     ip, ip, #4
    sub     r2, r2, ip
    lsls    ip, ip, #31             /* N = 1 byte, C = 2 bytes */
    strbmi  r1, [r3], #1
    strbcs  r1, [r3], #1
    strbcs  r1, [r3], #1
1:  mov     ip, r1
    cmp     r2, #64
    bhs     .Lfill_big
    subs    r2, r2, #16
    blo     3f
2:  stmia   r3!, {r1, ip}
    subs    r2, r2, #16
    stmia   r3!, {r1, ip}
    bhs     2b
3:  lsls    r2, r2, #29             /* C = 8, N = 4 */
    stmiacs r3!, {r1, ip}
    strmi   r1, [r3], #4
    lsls    r2, r2, #2              /* C = 2, N = 1 */
    strhcs  r1, [r3], #2
    strbmi  r1, [r3]
    bx      lr
.Lfill_lt4:
    lsls    r2, r2, #31             /* C = 2, N = 1 */
    strbcs  r1, [r3], #1
    strbcs  r1, [r3], #1
    strbmi  r1, [r3]
    bx      lr

/* n >= 64, dst word aligned: align dst to 32 B, then 32 B per STM */
.Lfill_big:
    push    {r4-r9}
    rsb     r4, r3, #0
    ands    r4, r4, #28
    sub     r2, r2, r4
    lsls    r4, r4, #28             /* C = 16, N = 8 */
    stmiacs r3!, {r1, ip}
    stmiacs r3!, {r1, ip}
    stmiami r3!, {r1, ip}
    lsls    r4, r4, #2              /* C = 4 */
    strcs   r1, [r3], #4
    mov     r4, r1
    mov     r5, r1
    mov     r6, r1
    mov     r7, r1
    mov     r8, r1
    mov     r9, r1
    sub     r2, r2, #32
1:  stmia   r3!, {r1, r4-r9, ip}
    subs    r2, r2, #32
    bhs     1b
    lsls    r4, r2, #28             /* C = 16, N = 8 */
    stmiacs r3!, {r1, r5-r7}
    stmiami r3!, {r1, ip}
    lsls    r4, r2, #30             /* C = 4, N = 2 */
    strcs   r1, [r3], #4
    strhmi  r1, [r3], #2
    tst     r2, #1
    strbne  r1, [r3]
    pop     {r4-r9}
    bx      lr
    .size memset, . - memset
    .size __aeabi_memset, . - __aeabi_memset
    .size __aeabi_memset4, . - __aeabi_memset4
    .size __aeabi_memset8, . - __aeabi_memset8
    .size __aeabi_memclr, . - __aeabi_memclr
    .size __aeabi_memclr4, . - __aeabi_memclr4
    .size __aeabi_memclr8, . - __aeabi_memclr8
