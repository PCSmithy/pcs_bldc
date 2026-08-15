/*
 * Native cooperative coroutine port for FreeRTOS V10.3.1 (SIL host build).
 *
 * Single OS thread; each task runs on its own coroutine. Cooperative: context
 * switches happen only at portYIELD / ISR-dispatch points. Identical semantics
 * to the Native-Fiber port, but the switch is a hand-rolled asm coroutine
 * (coro.*) rather than Win32 fibers — the non-Windows backend.
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type definitions (LLP64 host: long is 32-bit on Win64). */
#define portCHAR        char
#define portFLOAT       float
#define portDOUBLE      double
#define portLONG        long
#define portSHORT       short
#define portSTACK_TYPE  size_t
#define portBASE_TYPE   long
#define portPOINTER_SIZE_TYPE size_t

typedef portSTACK_TYPE StackType_t;
typedef long           BaseType_t;
typedef unsigned long  UBaseType_t;

#if ( configUSE_16_BIT_TICKS == 1 )
    typedef uint16_t TickType_t;
    #define portMAX_DELAY ( TickType_t ) 0xffff
#else
    typedef uint32_t TickType_t;
    #define portMAX_DELAY ( TickType_t ) 0xffffffffUL
    #define portTICK_TYPE_IS_ATOMIC 1
#endif

/* Architecture specifics. */
#define portSTACK_GROWTH      ( -1 )
#define portTICK_PERIOD_MS    ( ( TickType_t ) 1000 / configTICK_RATE_HZ )
#define portBYTE_ALIGNMENT    8
#define portINLINE            __inline

/* Scheduler / yield. A yield requested from inside a dispatched simulated
 * interrupt is deferred to the ISR bracket's exit, so the handler runs to
 * completion first; outside one it switches immediately. */
void vPortYield( void );
void vPortYieldFromIsr( void );
#define portYIELD()                       vPortYield()
#define portYIELD_WITHIN_API()            vPortYield()
#define portEND_SWITCHING_ISR( xSwitch )  do { if( xSwitch ) vPortYieldFromIsr(); } while( 0 )
#define portYIELD_FROM_ISR( xSwitch )     portEND_SWITCHING_ISR( xSwitch )

/* Critical sections + interrupt masking — cooperative single thread, so these
 * are counters/flags. They are what holds a due simulated interrupt PENDING
 * (xSilInterruptsMasked); the framework re-attempts it on the next grid step. */
void vPortEnterCritical( void );
void vPortExitCritical( void );
void vPortDisableInterrupts( void );
void vPortEnableInterrupts( void );
UBaseType_t uxPortSetInterruptMaskFromISR( void );
void vPortClearInterruptMaskFromISR( UBaseType_t uxSaved );

#define portENTER_CRITICAL()              vPortEnterCritical()
#define portEXIT_CRITICAL()               vPortExitCritical()
#define portDISABLE_INTERRUPTS()          vPortDisableInterrupts()
#define portENABLE_INTERRUPTS()           vPortEnableInterrupts()

#define portSET_INTERRUPT_MASK_FROM_ISR()       uxPortSetInterruptMaskFromISR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR( x )  vPortClearInterruptMaskFromISR( x )

/* Simulated-interrupt dispatch: the framework runs one handler in the
 * firmware context through the ISR bracket. pdFALSE = masked, held pending.
 * The kernel tick is one such handler — the port registers it with the
 * framework at scheduler start (docs/sil/sim-interrupts.md). */
BaseType_t xSilDispatchIsr( void ( * pxHandler )( void ) );
BaseType_t xSilInterruptsMasked( void );
void vSilSysTickHandler( void );

/* Task function macros. */
#define portTASK_FUNCTION_PROTO( vFunction, pvParameters ) \
    void vFunction( void *pvParameters )
#define portTASK_FUNCTION( vFunction, pvParameters ) \
    void vFunction( void *pvParameters )

#define portNOP()

#ifdef __cplusplus
}
#endif

#endif /* PORTMACRO_H */
