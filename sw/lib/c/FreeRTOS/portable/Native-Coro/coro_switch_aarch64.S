/*
 * AArch64 (AAPCS64) cooperative context switch + fresh-coroutine trampoline.
 *
 * A switch saves the callee-saved GP registers (x19-x28), the frame pointer
 * and link register (x29/x30), and the callee-saved SIMD registers (d8-d15) —
 * 160 bytes — onto the current stack, stores the resulting sp, then loads the
 * target's sp and restores the same set. Everything else is caller-saved and
 * already preserved by the C compiler around the coro_switch() call site.
 *
 *   void coro_asm_swap(void ** saveSp /x0/, void * newSp /x1/);
 *   void coro_asm_trampoline(void);   // x19=entry, x20=arg (set by coro_init)
 */
#if defined(__APPLE__)
#  define SYM(x) _##x
#else
#  define SYM(x) x
#endif

/* CORO_FRAME_BYTES — shared with coro.c so the frame size cannot drift. */
#include "coro_frame.h"

    .text

    .global SYM(coro_asm_swap)
    .p2align 2
SYM(coro_asm_swap):
    sub  sp, sp, #CORO_FRAME_BYTES
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mov  x2, sp
    str  x2, [x0]            /* *saveSp = sp */
    mov  sp, x1             /* sp = newSp  */
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #CORO_FRAME_BYTES
    ret

    .global SYM(coro_asm_trampoline)
    .p2align 2
SYM(coro_asm_trampoline):
    mov  x0, x20            /* arg   */
    blr  x19               /* entry(arg) — must not return */
    brk  #0                /* trap if it does */
