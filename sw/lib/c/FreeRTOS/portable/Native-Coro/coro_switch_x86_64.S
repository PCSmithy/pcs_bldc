/*
 * x86-64 (System V AMD64) cooperative context switch + fresh-coroutine
 * trampoline.
 *
 * A switch saves the six callee-saved GP registers (rbx, rbp, r12-r15) onto
 * the current stack and stores the resulting rsp, then loads the target's rsp
 * and restores the same set. System V has no callee-saved XMM registers, so
 * nothing else needs saving; everything else is caller-saved and already
 * preserved by the C compiler around the coro_switch() call site.
 *
 *   void coro_asm_swap(void ** saveSp /rdi/, void * newSp /rsi/);
 *   void coro_asm_trampoline(void);   // r12=entry, r13=arg (set by coro_init)
 */
#if defined(__APPLE__)
#  define SYM(x) _##x
#else
#  define SYM(x) x
#endif

    .text

    .globl SYM(coro_asm_swap)
    .p2align 4
SYM(coro_asm_swap):
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)         /* *saveSp = rsp */
    movq  %rsi, %rsp           /* rsp = newSp  */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret

    .globl SYM(coro_asm_trampoline)
    .p2align 4
SYM(coro_asm_trampoline):
    movq  %r13, %rdi           /* arg */
    callq *%r12                /* entry(arg) — must not return */
    ud2                        /* trap if it does */
