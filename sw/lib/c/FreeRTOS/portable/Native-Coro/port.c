/*
 * Native cooperative coroutine port for FreeRTOS V10.3.1 (SIL host build).
 *
 * Single OS thread. Each task runs on its own coroutine; the driver
 * ("framework") runs on the host thread's context (the "main" coroutine).
 * Context switches are coroutine swaps (a handful of register saves) at
 * cooperative points only (portYIELD, tick dispatch). The tick is
 * framework-driven: the driver calls vSilAdvanceTick() to advance one tick.
 * Quiescence (all tasks blocked -> idle runs) hands control back to the driver
 * via the idle hook calling vPortYieldToScheduler().
 *
 * This is the non-Windows counterpart of the Native-Fiber port: identical
 * cooperative semantics and SIL control ABI, but the context switch is a
 * hand-rolled asm coroutine (coro.*) instead of Win32 fibers, so it needs no
 * OS fiber API and no deprecated ucontext.
 */
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "coro.h"

/* Each task's coroutine gets its own stack, independent of the FreeRTOS stack
 * region (which only holds the ThreadState_t below). Generous for host use. */
#define PORT_CORO_STACK_BYTES   ( 64u * 1024u )

/* Per-task state stashed at the top of the task's FreeRTOS stack region. The
 * first member of TCB_t is pxTopOfStack, which holds whatever
 * pxPortInitialiseStack returns — we return &ThreadState, so the current
 * task's state is *(void**)pxCurrentTCB. */
typedef struct
{
    coro_t          coro;
    TaskFunction_t  pxCode;
    void *          pvParams;
} ThreadState_t;

/* pxCurrentTCB has external linkage in tasks.c (TCB_t* volatile). We only need
 * its first member (pxTopOfStack), so alias it as void*. */
extern void * volatile pxCurrentTCB;

#define prvCurrentThreadState()   ( ( ThreadState_t * ) *( ( void ** ) pxCurrentTCB ) )

static coro_t               mainCoro;
static volatile UBaseType_t uxCriticalNesting = 0;
static volatile BaseType_t  xSchedulerStopped = pdFALSE;

/* --- coroutine entry ----------------------------------------------------- */

static void prvCoroEntry( void * pvParam )
{
    ThreadState_t * ts = ( ThreadState_t * ) pvParam;
    ts->pxCode( ts->pvParams );
    /* Tasks must not return. Trap loudly. */
    configASSERT( pdFALSE );
    for( ;; )
    {
    }
}

/* --- port API ------------------------------------------------------------ */

StackType_t * pxPortInitialiseStack( StackType_t * pxTopOfStack,
                                     TaskFunction_t pxCode,
                                     void * pvParameters )
{
    /* Carve an aligned ThreadState_t out of the top of the stack region
     * (stack grows down, so the top is the highest address). */
    size_t addr = ( ( size_t ) pxTopOfStack - sizeof( ThreadState_t ) )
                  & ~( ( size_t ) portBYTE_ALIGNMENT - 1u );
    ThreadState_t * ts = ( ThreadState_t * ) addr;

    ts->pxCode   = pxCode;
    ts->pvParams = pvParameters;

    /* The task never returns, so the coroutine stack lives for the whole run;
     * not freed (matches the Native-Fiber CreateFiber model). */
    void * coroStack = malloc( PORT_CORO_STACK_BYTES );
    configASSERT( coroStack != NULL );
    coro_init( &ts->coro, coroStack, PORT_CORO_STACK_BYTES, prvCoroEntry, ts );

    /* Returned value becomes pxTopOfStack == TCB first member. */
    return ( StackType_t * ) ts;
}

BaseType_t xPortStartScheduler( void )
{
    coro_bootstrap( &mainCoro );
    xSchedulerStopped = pdFALSE;

    /* Kernel has selected the first task. Run the firmware until it reaches
     * quiescence (idle hook swaps back to mainCoro), then return so the driver
     * owns the outer loop. */
    coro_switch( &prvCurrentThreadState()->coro );

    return pdTRUE;
}

void vPortEndScheduler( void )
{
    xSchedulerStopped = pdTRUE;
}

/* Cooperative context switch: pick the next task and swap to its coroutine.
 * Called from a task (portYIELD / blocking API). */
void vPortYield( void )
{
    vTaskSwitchContext();
    coro_switch( &prvCurrentThreadState()->coro );
}

/* Called by the idle hook (vApplicationIdleHook) — quiescence handoff back to
 * the driver/main coroutine. */
void vPortYieldToScheduler( void )
{
    coro_switch( &mainCoro );
}

/* Framework-driven tick. Called from the main coroutine while the firmware is
 * quiescent. Advances one tick; if it readies a higher-priority task, swap into
 * the firmware and run it to the next quiescence. */
void vSilAdvanceTick( void )
{
    if( xTaskIncrementTick() != pdFALSE )
    {
        vTaskSwitchContext();
        coro_switch( &prvCurrentThreadState()->coro );
    }
}

/* --- critical sections (cooperative single thread -> counters only) ------- */

void vPortEnterCritical( void )
{
    uxCriticalNesting++;
}

void vPortExitCritical( void )
{
    configASSERT( uxCriticalNesting > 0 );
    uxCriticalNesting--;
}

void vPortDisableInterrupts( void )
{
}

void vPortEnableInterrupts( void )
{
}
