/*
 * Native cooperative FIBER port for FreeRTOS V10.3.1 (SIL host build).
 *
 * Single OS thread. Each task is a Windows fiber; the driver ("framework") is
 * the main fiber. Context switches are fiber swaps (~tens of ns) at cooperative
 * points only (portYIELD, ISR dispatch). Quiescence (all tasks blocked -> idle
 * runs) hands control back to the driver via the idle hook calling
 * vPortYieldToScheduler().
 *
 * Validated by the D1 spike (sw/sil/spike/d1-tick): deterministic + ~5000x
 * realtime. See docs/sil/freertos-tick.md, docs/sil/performance.md.
 *
 * Every interrupt — the kernel tick included — arrives the same way: the port
 * registers its systick with the framework at scheduler start, and the framework
 * calls xSilDispatchIsr for each handler due on the sim grid, bracketed by ISR
 * entry/exit so ...FromISR wakeups and portYIELD_FROM_ISR behave as on hardware
 * (docs/sil/sim-interrupts.md).
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
#include "SIL_irq.h"

#define PORT_FIBER_STACK_BYTES   ( 64u * 1024u )

/* Same-step dispatch order only (lower value first, no preemption). The kernel
 * tick sits at the bottom of the ladder, as SysTick does on Cortex-M: a
 * control-loop ISR and a peripheral ISR (sim HW_USB uses 8) both outrank it. */
#define PORT_SYSTICK_PRIORITY    ( 15u )

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
    /* This context's critical nesting and interrupt mask, saved while it is
     * switched out. On hardware BASEPRI/PRIMASK are part of a task's saved
     * context; the kernel's blocking APIs (ulTaskNotifyTake, ...) yield from
     * INSIDE a critical section, so a global counter would leave every other
     * context looking masked. */
    UBaseType_t     uxCriticalNesting;
    UBaseType_t     uxInterruptMask;
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
/* The RUNNING context's critical nesting and interrupt mask (each switched-out
 * context keeps its own in its ThreadState_t; the driver/main fiber's live in
 * the uxMain* pair). */
static volatile UBaseType_t uxCriticalNesting     = 0;
static volatile UBaseType_t uxMainCriticalNesting = 0;
static volatile BaseType_t  xSchedulerStopped     = pdFALSE;

/* --- simulated-interrupt bookkeeping (docs/sil/sim-interrupts.md) --------- */

/* Cooperative stand-in for PRIMASK, set by portDISABLE_INTERRUPTS and the
 * FromISR mask macros. Together with uxCriticalNesting it is what holds a due
 * simulated interrupt pending instead of dropping it. */
static volatile UBaseType_t uxInterruptMask     = 0;
static volatile UBaseType_t uxMainInterruptMask = 0;
/* Non-zero while a dispatched handler is running (no nesting is modelled). */
static volatile UBaseType_t uxIsrNesting      = 0;
/* A ...FromISR call asked for a context switch; honored at bracket exit. */
static volatile BaseType_t  xIsrYieldPending  = pdFALSE;
/* This port's systick entry in the framework's interrupt table. */
static int32_t              lSysTickHandle    = SIL_IRQ_HANDLE_INVALID;

/* --- context switching --------------------------------------------------- */

/* Swap to the task the kernel has selected, carrying the per-context critical
 * nesting and interrupt mask across: the outgoing context's are stashed in the
 * puxOut pair, the incoming task's become current. Every switch into a task
 * goes through here. */
static void prvSwitchToSelectedTask( volatile UBaseType_t * puxOutNesting,
                                     volatile UBaseType_t * puxOutMask )
{
    ThreadState_t * const pxNext = prvCurrentThreadState();

    *puxOutNesting    = uxCriticalNesting;
    *puxOutMask       = uxInterruptMask;
    uxCriticalNesting = pxNext->uxCriticalNesting;
    uxInterruptMask   = pxNext->uxInterruptMask;
    SwitchToFiber( pxNext->pvFiber );
}

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

    ts->pxCode            = pxCode;
    ts->pvParams          = pvParameters;
    ts->uxCriticalNesting = 0;
    ts->uxInterruptMask   = 0;
    ts->pvFiber           = CreateFiber( PORT_FIBER_STACK_BYTES, prvFiberEntry, ts );
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

    /* vTaskStartScheduler masks interrupts before calling here; on hardware the
     * first task's context restore clears PRIMASK. Mirror that, or every
     * simulated interrupt would read as masked forever. */
    uxInterruptMask   = 0;
    uxIsrNesting      = 0;
    xIsrYieldPending  = pdFALSE;
    uxCriticalNesting = 0;

    /* The kernel tick is a plain interrupt-table entry, so the port registers it
     * here, before the first task runs. The seam does no clock arithmetic — the
     * caller hands it a period. With no framework hooks installed this no-ops and
     * no tick ever fires, which is what a standalone/Unity run wants. */
    lSysTickHandle = SIL_irq_registerPeriodic( vSilSysTickHandler,
                                               1000000u / configTICK_RATE_HZ,
                                               PORT_SYSTICK_PRIORITY );

    /* Kernel has selected the first task. Run the firmware until it reaches
     * quiescence (idle hook swaps back to pvMainFiber), then return so the
     * driver owns the outer loop. */
    prvSwitchToSelectedTask( &uxMainCriticalNesting, &uxMainInterruptMask );

    return pdTRUE;
}

void vPortEndScheduler( void )
{
    void * const pvCurrent = GetCurrentFiber();

    /* Teardown runs on the driver/main fiber (the framework calls it while the
     * firmware is quiescent), never from a task. */
    configASSERT( pvCurrent == pvMainFiber );

    xSchedulerStopped = pdTRUE;

    /* Drop the systick entry so a rebooted image registers a fresh one. */
    SIL_irq_cancel( lSysTickHandle );
    lSysTickHandle = SIL_IRQ_HANDLE_INVALID;

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
    ThreadState_t * const pxOut = prvCurrentThreadState();

    vTaskSwitchContext();
    prvSwitchToSelectedTask( &pxOut->uxCriticalNesting, &pxOut->uxInterruptMask );
}

/* Called by the idle hook (vApplicationIdleHook) — quiescence handoff back to
 * the driver/main fiber. */
void vPortYieldToScheduler( void )
{
    ThreadState_t * const pxOut = prvCurrentThreadState();

    pxOut->uxCriticalNesting = uxCriticalNesting;
    pxOut->uxInterruptMask   = uxInterruptMask;
    uxCriticalNesting = uxMainCriticalNesting;
    uxInterruptMask   = uxMainInterruptMask;
    SwitchToFiber( pvMainFiber );
}

/* --- simulated interrupts ------------------------------------------------ */

/* The kernel tick handler, dispatched from the framework's interrupt table like
 * any other. Shaped exactly as a real port's SysTick_Handler: advance the tick,
 * and if that readied a higher-priority task, let the bracket's exit tail-chain
 * into it. Non-static so a scenario can resolve it by name from DWARF. */
void vSilSysTickHandler( void )
{
    if( xTaskIncrementTick() != pdFALSE )
    {
        vPortYieldFromIsr();
    }
}

/* Whether firmware interrupts are currently masked — a critical section or an
 * explicit portDISABLE_INTERRUPTS / FromISR mask. */
BaseType_t xSilInterruptsMasked( void )
{
    return ( ( uxCriticalNesting > 0 ) || ( uxInterruptMask != 0 ) ) ? pdTRUE : pdFALSE;
}

/* Run one simulated interrupt handler in the firmware context, bracketed by ISR
 * entry/exit so ...FromISR bookkeeping holds. Runs on the driver fiber (the
 * firmware is quiescent) — the closest analogue of the handler mode an ISR runs
 * in on hardware, which is no task's stack either. Returns pdFALSE without
 * calling the handler when interrupts are masked; the framework then holds the
 * interrupt pending and re-attempts on the next step. */
BaseType_t xSilDispatchIsr( void ( * pxHandler )( void ) )
{
    BaseType_t xDispatched = pdFALSE;

    if( ( pxHandler != NULL ) &&
        ( pvMainFiber != NULL ) &&
        ( xSchedulerStopped == pdFALSE ) &&
        ( xSilInterruptsMasked() == pdFALSE ) )
    {
        uxIsrNesting     = 1;
        xIsrYieldPending = pdFALSE;
        pxHandler();
        uxIsrNesting = 0;

        /* A FromISR wakeup deferred its switch to here: run the readied task
         * now, before returning to quiescence — the tail-chained context switch
         * a real ISR exit performs. */
        if( xIsrYieldPending != pdFALSE )
        {
            xIsrYieldPending = pdFALSE;
            vTaskSwitchContext();
            prvSwitchToSelectedTask( &uxMainCriticalNesting, &uxMainInterruptMask );
        }
        xDispatched = pdTRUE;
    }

    return xDispatched;
}

/* portYIELD_FROM_ISR / portEND_SWITCHING_ISR. Inside a dispatch bracket the
 * switch is DEFERRED to bracket exit, so the handler always runs to completion
 * as it would on hardware; outside one it is an ordinary cooperative yield. */
void vPortYieldFromIsr( void )
{
    if( uxIsrNesting > 0 )
    {
        xIsrYieldPending = pdTRUE;
    }
    else
    {
        vPortYield();
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
    uxInterruptMask = 1;
}

void vPortEnableInterrupts( void )
{
    uxInterruptMask = 0;
}

UBaseType_t uxPortSetInterruptMaskFromISR( void )
{
    const UBaseType_t uxSaved = uxInterruptMask;
    uxInterruptMask = 1;
    return uxSaved;
}

void vPortClearInterruptMaskFromISR( UBaseType_t uxSaved )
{
    uxInterruptMask = uxSaved;
}
