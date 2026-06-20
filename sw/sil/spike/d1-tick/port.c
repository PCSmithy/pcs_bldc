/*
 * D1 spike — native cooperative FIBER port for FreeRTOS V10.3.1.
 *
 * Single OS thread. Each task is a Windows fiber; the driver ("framework")
 * is the main fiber. Context switches are fiber swaps (~tens of ns) at
 * cooperative points only (portYIELD, tick dispatch). The tick is
 * framework-driven: the driver calls vSilAdvanceTick() to advance one tick.
 * Quiescence (all tasks blocked → idle runs) hands control back to the driver
 * via the idle hook calling vPortYieldToScheduler().
 *
 * Proves the D1 thesis: deterministic (cooperative, defined switch points)
 * AND fast (fiber swaps, no OS-thread signaling). See docs/sil/freertos-tick.md.
 */
#include <windows.h>

#include "FreeRTOS.h"
#include "task.h"

#define SPIKE_FIBER_STACK_BYTES   ( 64u * 1024u )

/* Per-task state stashed at the top of the task's FreeRTOS stack region.
 * The first member of TCB_t is pxTopOfStack, which holds whatever
 * pxPortInitialiseStack returns — we return &ThreadState, so the current
 * task's state is *(void**)pxCurrentTCB. */
typedef struct
{
    void *          pvFiber;
    TaskFunction_t  pxCode;
    void *          pvParams;
} ThreadState_t;

/* pxCurrentTCB has external linkage in tasks.c (TCB_t* volatile). We only
 * need its first member (pxTopOfStack), so alias it as void*. */
extern void * volatile pxCurrentTCB;

#define prvCurrentThreadState()   ( ( ThreadState_t * ) *( ( void ** ) pxCurrentTCB ) )

static void *               pvMainFiber       = NULL;
static volatile UBaseType_t uxCriticalNesting = 0;
static volatile BaseType_t  xSchedulerStopped = pdFALSE;

/* --- fiber entry --------------------------------------------------------- */

static void CALLBACK prvFiberEntry( void * lpParam )
{
    ThreadState_t * ts = ( ThreadState_t * ) lpParam;
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
    ts->pvFiber  = CreateFiber( SPIKE_FIBER_STACK_BYTES, prvFiberEntry, ts );
    configASSERT( ts->pvFiber != NULL );

    /* Returned value becomes pxTopOfStack == TCB first member. */
    return ( StackType_t * ) ts;
}

BaseType_t xPortStartScheduler( void )
{
    pvMainFiber = ConvertThreadToFiber( NULL );
    configASSERT( pvMainFiber != NULL );

    xSchedulerStopped = pdFALSE;

    /* Kernel has selected the first task. Run the firmware until it reaches
     * quiescence (idle hook swaps back to pvMainFiber), then return so the
     * driver owns the outer loop. */
    SwitchToFiber( prvCurrentThreadState()->pvFiber );

    return pdTRUE;
}

void vPortEndScheduler( void )
{
    xSchedulerStopped = pdTRUE;
}

/* Cooperative context switch: pick the next task and swap to its fiber.
 * Called from a task fiber (portYIELD / blocking API). */
void vPortYield( void )
{
    vTaskSwitchContext();
    SwitchToFiber( prvCurrentThreadState()->pvFiber );
}

/* Called by the idle hook (vApplicationIdleHook) — quiescence handoff back
 * to the driver/main fiber. */
void vPortYieldToScheduler( void )
{
    SwitchToFiber( pvMainFiber );
}

/* Framework-driven tick. Called from the main fiber while the firmware is
 * quiescent. Advances one tick; if it readies a higher-priority task, swap
 * into the firmware and run it to the next quiescence. */
void vSilAdvanceTick( void )
{
    if( xTaskIncrementTick() != pdFALSE )
    {
        vTaskSwitchContext();
        SwitchToFiber( prvCurrentThreadState()->pvFiber );
    }
}

/* --- critical sections (cooperative single thread → counters only) ------- */

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
