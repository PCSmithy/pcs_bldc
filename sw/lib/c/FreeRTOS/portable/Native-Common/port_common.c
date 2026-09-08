/*
 * Native cooperative port for FreeRTOS V10.3.1 (SIL host build) — the
 * host-independent half. The context-switch primitive is injected through
 * port_backend.h (Win32 fibers, or a hand-rolled asm coroutine).
 *
 * Single OS thread. Each task runs on its own backend context; the driver
 * ("framework") runs on the host thread's own. Context switches happen at
 * cooperative points only (portYIELD, ISR dispatch). Quiescence — every task
 * blocked, so idle runs — hands control back to the driver via the idle hook
 * calling vPortYieldToScheduler().
 *
 * Every interrupt, the kernel tick included, arrives the same way: the port
 * registers its systick with the framework at scheduler start, and the framework
 * calls xSilDispatchIsr for each handler due on the sim grid, bracketed by ISR
 * entry/exit so ...FromISR wakeups and portYIELD_FROM_ISR behave as on hardware
 * (docs/sil/sim-interrupts.md).
 *
 * The port is restartable and shareable per thread: its own allocations (task
 * contexts, systick entry, any thread conversion the backend owns) are released
 * by vPortEndScheduler. The kernel heap is not — deleting the tasks is the
 * driver's job.
 *
 * Fidelity limit: usStackDepth is ignored. Every task gets a fixed host stack,
 * so a stack overflow that would fire on the STM32G431 cannot reproduce here.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "SIL_irq.h"
#include "port_backend.h"

/* Same-step dispatch order only (lower value first, no preemption). The kernel
 * tick sits at the bottom of the ladder, as SysTick does on Cortex-M: a
 * control-loop ISR and a peripheral ISR (sim HW_USB uses 8) both outrank it. */
#define PORT_SYSTICK_PRIORITY     ( 15u )

/* Upper bound on task contexts tracked for teardown (idle + timer + app tasks). */
#define PORT_MAX_TASK_CONTEXTS    ( 32u )

/* pxCurrentTCB has external linkage in tasks.c (TCB_t* volatile). We only need
 * its first member (pxTopOfStack), so alias it as void*. */
extern void * volatile pxCurrentTCB;

#define prvCurrentThreadState()   ( ( ThreadState_t * ) *( ( void ** ) pxCurrentTCB ) )

/* Every task context created in pxPortInitialiseStack, for teardown release. */
static ThreadState_t *      pxTaskStates[ PORT_MAX_TASK_CONTEXTS ];
static UBaseType_t          uxTaskStateCount      = 0;
/* The RUNNING context's critical nesting and interrupt mask (each switched-out
 * context keeps its own in its ThreadState_t; the driver's live in uxMain*). */
static volatile UBaseType_t uxCriticalNesting     = 0;
static volatile UBaseType_t uxMainCriticalNesting = 0;
static volatile BaseType_t  xSchedulerStopped     = pdFALSE;

/* --- simulated-interrupt bookkeeping (docs/sil/sim-interrupts.md) --------- */

/* Cooperative stand-in for PRIMASK, set by portDISABLE_INTERRUPTS and the
 * FromISR mask macros, switched with the context as PRIMASK is on hardware. */
static volatile UBaseType_t uxInterruptMask     = 0;
static volatile UBaseType_t uxMainInterruptMask = 0;
/* Non-zero while a dispatched handler is running (no nesting is modelled). */
static volatile UBaseType_t uxIsrNesting        = 0;
/* A ...FromISR call asked for a context switch; honored at bracket exit. */
static volatile BaseType_t  xIsrYieldPending    = pdFALSE;
/* This port's systick entry in the framework's interrupt table. */
static int32_t              lSysTickHandle      = SIL_IRQ_HANDLE_INVALID;

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
    vPortBackendSwitchTo( pxNext );
}

void vPortCommonTaskEntry( void * pvState )
{
    ThreadState_t * const ts = ( ThreadState_t * ) pvState;

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
    /* Carve an aligned ThreadState_t out of the top of the stack region (stack
     * grows down); the pointer returned here becomes the TCB's pxTopOfStack,
     * which is what prvCurrentThreadState() reads back. */
    const size_t addr = ( ( size_t ) pxTopOfStack - sizeof( ThreadState_t ) )
                        & ~( ( size_t ) portBYTE_ALIGNMENT - 1u );
    ThreadState_t * const ts = ( ThreadState_t * ) addr;

    ts->pxCode            = pxCode;
    ts->pvParams          = pvParameters;
    ts->uxCriticalNesting = 0;
    ts->uxInterruptMask   = 0;
    vPortBackendCreateContext( ts );

    configASSERT( uxTaskStateCount < PORT_MAX_TASK_CONTEXTS );
    pxTaskStates[ uxTaskStateCount ] = ts;
    uxTaskStateCount++;

    return ( StackType_t * ) ts;
}

BaseType_t xPortStartScheduler( void )
{
    vPortBackendStart();

    xSchedulerStopped = pdFALSE;

    /* vTaskStartScheduler masks interrupts before calling here; on hardware the
     * first task's context restore clears PRIMASK. Mirror that, or every
     * simulated interrupt would read as masked forever. */
    uxInterruptMask   = 0;
    uxIsrNesting      = 0;
    xIsrYieldPending  = pdFALSE;
    uxCriticalNesting = 0;

    /* The kernel tick is a plain interrupt-table entry, so the port registers it
     * here, before the first task runs. Cancelling first keeps a start without an
     * intervening teardown from orphaning the previous entry (no-op on an invalid
     * handle). With no framework hooks installed this no-ops and no tick ever
     * fires, which is what a standalone/Unity run wants. */
    SIL_irq_cancel( lSysTickHandle );
    lSysTickHandle = SIL_irq_registerPeriodic( vSilSysTickHandler,
                                               1000000u / configTICK_RATE_HZ,
                                               PORT_SYSTICK_PRIORITY );

    /* Run the firmware until it reaches quiescence, then return so the driver
     * owns the outer loop. */
    prvSwitchToSelectedTask( &uxMainCriticalNesting, &uxMainInterruptMask );

    return pdTRUE;
}

void vPortEndScheduler( void )
{
    /* Teardown runs on the driver context (the framework calls it while the
     * firmware is quiescent), never from a task — the contexts released below
     * include every task's. */
    configASSERT( xPortBackendOnDriver() != pdFALSE );

    xSchedulerStopped = pdTRUE;

    /* Drop the systick entry so a rebooted image registers a fresh one. */
    SIL_irq_cancel( lSysTickHandle );
    lSysTickHandle = SIL_IRQ_HANDLE_INVALID;

    for( UBaseType_t i = 0; i < uxTaskStateCount; i++ )
    {
        vPortBackendDestroyContext( pxTaskStates[ i ] );
        pxTaskStates[ i ] = NULL;
    }
    uxTaskStateCount = 0;

    vPortBackendEnd();
}

/* Cooperative context switch: pick the next task and swap to its context.
 * Called from a task (portYIELD / blocking API). */
void vPortYield( void )
{
    ThreadState_t * const pxOut = prvCurrentThreadState();

    vTaskSwitchContext();
    /* vTaskSwitchContext may re-select the running task (scheduler suspended, or
     * sole ready task at this priority); switching a context to itself is UB. */
    if( prvCurrentThreadState() != pxOut )
    {
        prvSwitchToSelectedTask( &pxOut->uxCriticalNesting, &pxOut->uxInterruptMask );
    }
}

/* Called by the idle hook (vApplicationIdleHook) — quiescence handoff back to
 * the driver context. */
void vPortYieldToScheduler( void )
{
    ThreadState_t * const pxOut = prvCurrentThreadState();

    pxOut->uxCriticalNesting = uxCriticalNesting;
    pxOut->uxInterruptMask   = uxInterruptMask;
    uxCriticalNesting = uxMainCriticalNesting;
    uxInterruptMask   = uxMainInterruptMask;
    vPortBackendSwitchToDriver();
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

/* Whether the RUNNING context has interrupts masked — a critical section or an
 * explicit portDISABLE_INTERRUPTS / FromISR mask. */
static BaseType_t prvInterruptsMasked( void )
{
    return ( ( uxCriticalNesting > 0 ) || ( uxInterruptMask != 0 ) ) ? pdTRUE : pdFALSE;
}

/* Run one simulated interrupt handler in the firmware context, bracketed by ISR
 * entry/exit so ...FromISR bookkeeping holds. Runs on the driver context — the
 * closest analogue of the handler mode an ISR runs in on hardware, which is no
 * task's stack either.
 *
 * Dispatch is quiescence-only, so the mask test reads the driver's pair, which is
 * clear whenever the firmware is quiescent: it guards against a handler that left
 * a critical section or mask unbalanced rather than deferring a due interrupt. A
 * task's portDISABLE_INTERRUPTS therefore holds nothing pending — an interrupt
 * cannot be delivered mid-task at all. */
BaseType_t xSilDispatchIsr( void ( * pxHandler )( void ) )
{
    BaseType_t xDispatched = pdFALSE;

    if( ( pxHandler != NULL ) &&
        ( uxIsrNesting == 0 ) &&
        ( xPortBackendIsLive() != pdFALSE ) &&
        ( xSchedulerStopped == pdFALSE ) &&
        ( prvInterruptsMasked() == pdFALSE ) )
    {
        /* The tail-chain below saves the running context into the uxMain* pair,
         * which is only correct if that context really is the driver's. */
        configASSERT( xPortBackendOnDriver() != pdFALSE );

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
