/*
 * Saved-frame geometry shared by coro.c and coro_switch_<arch>.S.
 *
 * These constants describe the callee-saved register frame a context switch
 * pushes onto a coroutine's stack, and the three slots coro_init() populates
 * for a fresh coroutine (entry function, its argument, and the return address
 * the first switch-in `ret`s into — the trampoline). Keeping them in one header
 * that BOTH the C (which builds a fresh frame) and the asm (which saves and
 * restores it) include means the frame size and those slot offsets cannot drift
 * apart silently — a mismatch would corrupt entry/arg/return on the first
 * switch into a task.
 *
 * Values are plain decimal integers with NO type suffix so the same macros
 * expand cleanly in C and in the GNU assembler (`sub sp, sp, #CORO_FRAME_BYTES`
 * must become `#160`, not `#160u`). coro.c adds _Static_assert checks on these.
 */
#ifndef PCS_CORO_FRAME_H
#define PCS_CORO_FRAME_H

#if defined( __aarch64__ )

/* coro_switch_aarch64.S saves x19-x28, x29/x30, d8-d15 -> 160 bytes.
 * Layout: x19@0 x20@8 ... x29@80 x30@88 ... d15@152. */
#  define CORO_FRAME_BYTES  160
#  define CORO_OFF_ENTRY    0    /* x19 */
#  define CORO_OFF_ARG      8    /* x20 */
#  define CORO_OFF_RET      88   /* x30 (link register) */

#elif defined( __x86_64__ )

/* coro_switch_x86_64.S pushes rbp, rbx, r12-r15 (48 bytes) then the return
 * address (8) -> 56. FRAME_BYTES is 56 (== 8 mod 16) so an aligned stack top
 * leaves the trampoline's `call` 16-byte aligned per the System V ABI. */
#  define CORO_FRAME_BYTES  56
#  define CORO_OFF_ENTRY    24   /* r12 */
#  define CORO_OFF_ARG      16   /* r13 */
#  define CORO_OFF_RET      48   /* return address */

#endif

#endif /* PCS_CORO_FRAME_H */
