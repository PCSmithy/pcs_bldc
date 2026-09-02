/*
 * Backend seam for the native cooperative FreeRTOS ports (SIL host build).
 *
 * port_common.c carries the whole port — kernel entry points, ISR bracket,
 * critical sections — and calls out to the handful of functions below for the
 * one thing that genuinely differs per host: the context-switch primitive
 * (Win32 fibers in Native-Fiber, a hand-rolled asm coroutine in Native-Coro).
 * Exactly one backend translation unit is compiled per build, so these bind
 * statically — no vtable, no indirection.
 */
#ifndef PORT_BACKEND_H
#define PORT_BACKEND_H

#include "FreeRTOS.h"
#include "task.h"

/* Each task gets its own host stack, independent of the FreeRTOS stack region
 * (which only holds the ThreadState_t below). Generous for host use. */
#define PORT_TASK_STACK_BYTES   ( 64u * 1024u )

/* Per-task state stashed at the top of the task's FreeRTOS stack region. The
 * first member of TCB_t is pxTopOfStack, which holds whatever
 * pxPortInitialiseStack returns — we return &ThreadState, so the current
 * task's state is *(void**)pxCurrentTCB. */
typedef struct
{
    void *          pvContext;   /* backend handle: fiber, or coro_t * */
    TaskFunction_t  pxCode;
    void *          pvParams;
    /* This context's critical nesting and interrupt mask, saved while it is
     * switched out. On hardware BASEPRI/PRIMASK are part of a task's saved
     * context; the kernel's blocking APIs (ulTaskNotifyTake, ...) yield from
     * INSIDE a critical section, so a global counter would leave every other
     * context looking masked. */
    UBaseType_t     uxCriticalNesting;
    UBaseType_t     uxInterruptMask;
} ThreadState_t;

/* Task trampoline, provided by port_common.c: a context created by
 * vPortBackendCreateContext must begin executing vPortCommonTaskEntry( ts ). */
void vPortCommonTaskEntry( void * pvState );

/* Create / release a task's backend context. Create stores its handle in
 * ts->pvContext; destroy is never called on the running context. */
void vPortBackendCreateContext( ThreadState_t * const ts );
void vPortBackendDestroyContext( ThreadState_t * const ts );

/* Claim / release the driver context — the one the framework calls in on. */
void vPortBackendStart( void );
void vPortBackendEnd( void );

/* Switch the running context to a task, or back to the driver. */
void vPortBackendSwitchTo( ThreadState_t * const ts );
void vPortBackendSwitchToDriver( void );

/* pdTRUE between vPortBackendStart and vPortBackendEnd. */
BaseType_t xPortBackendIsLive( void );

/* pdTRUE when the driver context is running. Also pdTRUE before start, where
 * the backend has nothing to compare against. */
BaseType_t xPortBackendOnDriver( void );

#endif /* PORT_BACKEND_H */
