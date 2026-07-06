/*
 * Minimal cooperative coroutine primitive for the SIL host build — a
 * hand-rolled equivalent of Win32 fibers with no OS or deprecated-ucontext
 * dependency. The full callee-saved register context is saved on each
 * coroutine's own stack across a switch; a coro_t holds only the stack pointer.
 *
 * Backends live in coro_switch_<arch>.S (AArch64 today; x86-64 to follow).
 */
#ifndef PCS_CORO_H
#define PCS_CORO_H

#include <stddef.h>

typedef struct
{
    void * sp;   /* saved stack pointer of the suspended context */
} coro_t;

/* Bind `mainCoro` as the running coroutine (the host thread's own context).
 * Its sp is captured on the first switch away. Call once before any switch. */
void coro_bootstrap( coro_t * mainCoro );

/* Prepare `c` to run entry(arg) on [stackMem, stackMem+stackBytes). The first
 * coro_switch into `c` begins executing entry; entry must never return. */
void coro_init( coro_t * c, void * stackMem, size_t stackBytes,
                void ( * entry )( void * arg ), void * arg );

/* Suspend the running coroutine and resume `to`. The current coroutine is
 * implicit (as with Win32 SwitchToFiber). */
void coro_switch( coro_t * to );

#endif /* PCS_CORO_H */
