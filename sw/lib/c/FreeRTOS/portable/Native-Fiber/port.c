/*
 * Native cooperative FIBER port for FreeRTOS V10.3.1 (SIL host build).
 *
 * Single OS thread. Each task is a Windows fiber; the driver ("framework") is
 * the main fiber. Context switches are fiber swaps (~tens of ns) at cooperative
 * points only (portYIELD, tick dispatch). The tick is framework-driven: the
 * driver calls vSilAdvanceTick() to advance one tick. Quiescence (all tasks
 * blocked -> idle runs) hands control back to the driver via the idle hook
 * calling vPortYieldToScheduler().
 *
 * Validated by the D1 spike (sw/sil/spike/d1-tick): deterministic + ~5000x
 * realtime. See docs/sil/freertos-tick.md, docs/sil/performance.md.
 *
 * The port is restartable and shareable per thread. It converts the thread to a
 * fiber only when the thread is not already one (a second firmware image on the
 * same thread borrows the existing conversion), and vPortEndScheduler deletes the
 * task fibers and un-converts when it owns the conversion — so repeated boots on
 * one thread and multi-image worlds both work.
 *
 * Windows-only for now; macOS (ucontext / asm) is a later roadmap item — its
 * teardown needs the equivalent un-convert on the owning image.
 */
#include <windows.h>

#include "FreeRTOS.h"
#include "task.h"

#define PORT_FIBER_STACK_BYTES   ( 64u * 1024u )

/* Upper bound on task fibers tracked for teardown (idle + timer + app tasks). */
#define PORT_MAX_TASK_FIBERS     ( 32u )

/* Per-task state stashed at the top of the task's FreeRTOS stack region. The
 * first member of TCB_t is pxTopOfStack, which holds whatever
 * pxPortInitialiseStack returns — we return &ThreadState, so the current
 * task's state is *(void**)pxCurrentTCB. */
typedef struct
{
    void *          pvFiber;
    TaskFunction_t  pxCode;
    void *          pvParams;
} ThreadState_t;

/* pxCurrentTCB has external linkage in tasks.c (TCB_t* volatile). We only need
 * its first member (pxTopOfStack), so alias it as void*. */
extern void * volatile pxCurrentTCB;

#define prvCurrentThreadState()   ( ( ThreadState_t * ) *( ( void ** ) pxCurrentTCB ) )

static void *               pvMainFiber       = NULL;
/* pdTRUE when this port instance converted the thread to a fiber (so teardown
 * un-converts it); pdFALSE when it borrowed a conversion another image made. */
static BaseType_t           xOwnsConversion   = pdFALSE;
/* Every task fiber created in pxPortInitialiseStack, for teardown deletion. */
static void *               pvTaskFibers[ PORT_MAX_TASK_FIBERS ];
static UBaseType_t          uxTaskFiberCount  = 0;
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
    ts->pvFiber  = CreateFiber( PORT_FIBER_STACK_BYTES, prvFiberEntry, ts );
    configASSERT( ts->pvFiber != NULL );

    /* Track the fiber so vPortEndScheduler can delete it at teardown. */
    configASSERT( uxTaskFiberCount < PORT_MAX_TASK_FIBERS );
    pvTaskFibers[ uxTaskFiberCount ] = ts->pvFiber;
    uxTaskFiberCount++;

    /* Returned value becomes pxTopOfStack == TCB first member. */
    return ( StackType_t * ) ts;
}

BaseType_t xPortStartScheduler( void )
{
    /* Own the conversion only if the thread is not already a fiber. A second
     * firmware image on a thread another image converted borrows that existing
     * conversion (its own port statics record ownership independently). */
    if( IsThreadAFiber() == FALSE )
    {
        pvMainFiber = ConvertThreadToFiber( NULL );
        configASSERT( pvMainFiber != NULL );
        xOwnsConversion = pdTRUE;
    }
    else
    {
        pvMainFiber = GetCurrentFiber();
        xOwnsConversion = pdFALSE;
    }

    xSchedulerStopped = pdFALSE;

    /* Kernel has selected the first task. Run the firmware until it reaches
     * quiescence (idle hook swaps back to pvMainFiber), then return so the
     * driver owns the outer loop. */
    SwitchToFiber( prvCurrentThreadState()->pvFiber );

    return pdTRUE;
}

void vPortEndScheduler( void )
{
    void * const pvCurrent = GetCurrentFiber();

    /* Teardown runs on the driver/main fiber (the framework calls it while the
     * firmware is quiescent), never from a task. */
    configASSERT( pvCurrent == pvMainFiber );

    xSchedulerStopped = pdTRUE;

    /* Delete every task fiber (never the running one) and clear the bookkeeping. */
    for( UBaseType_t i = 0; i < uxTaskFiberCount; i++ )
    {
        if( ( pvTaskFibers[ i ] != NULL ) && ( pvTaskFibers[ i ] != pvCurrent ) )
        {
            DeleteFiber( pvTaskFibers[ i ] );
        }
        pvTaskFibers[ i ] = NULL;
    }
    uxTaskFiberCount = 0;

    /* Un-convert only the image that owns the conversion, so a fresh
     * ConvertThreadToFiber on the SAME thread succeeds on the next boot; an image
     * that borrowed a conversion leaves it intact for the owner to release. */
    if( xOwnsConversion != pdFALSE )
    {
        const BOOL xConverted = ConvertFiberToThread();
        configASSERT( xConverted != FALSE );
        ( void ) xConverted;
        xOwnsConversion = pdFALSE;
    }
    pvMainFiber = NULL;
}

/* Cooperative context switch: pick the next task and swap to its fiber. Called
 * from a task fiber (portYIELD / blocking API). */
void vPortYield( void )
{
    vTaskSwitchContext();
    SwitchToFiber( prvCurrentThreadState()->pvFiber );
}

/* Called by the idle hook (vApplicationIdleHook) — quiescence handoff back to
 * the driver/main fiber. */
void vPortYieldToScheduler( void )
{
    SwitchToFiber( pvMainFiber );
}

/* Framework-driven tick. Called from the main fiber while the firmware is
 * quiescent. Advances one tick; if it readies a higher-priority task, swap into
 * the firmware and run it to the next quiescence. */
void vSilAdvanceTick( void )
{
    if( xTaskIncrementTick() != pdFALSE )
    {
        vTaskSwitchContext();
        SwitchToFiber( prvCurrentThreadState()->pvFiber );
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
