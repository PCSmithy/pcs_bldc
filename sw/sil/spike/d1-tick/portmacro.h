/*
 * D1 spike — portmacro.h for the native cooperative fiber port.
 *
 * Single OS thread; each task is a Windows fiber. Cooperative: context
 * switches happen only at portYIELD / tick dispatch points. Modeled on the
 * FreeRTOS MSVC-MingW simulator port's macro layer, but fiber-based.
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

/* Scheduler / yield. */
void vPortYield( void );
#define portYIELD()                       vPortYield()
#define portYIELD_WITHIN_API()            vPortYield()
#define portEND_SWITCHING_ISR( xSwitch )  do { if( xSwitch ) vPortYield(); } while( 0 )
#define portYIELD_FROM_ISR( xSwitch )     portEND_SWITCHING_ISR( xSwitch )

/* Critical sections — cooperative single thread, so these are a nesting
 * counter that masks tick dispatch (the only "interrupt" in the spike). */
void vPortEnterCritical( void );
void vPortExitCritical( void );
void vPortDisableInterrupts( void );
void vPortEnableInterrupts( void );

#define portENTER_CRITICAL()              vPortEnterCritical()
#define portEXIT_CRITICAL()               vPortExitCritical()
#define portDISABLE_INTERRUPTS()          vPortDisableInterrupts()
#define portENABLE_INTERRUPTS()           vPortEnableInterrupts()

#define portSET_INTERRUPT_MASK_FROM_ISR()       ( 0 )
#define portCLEAR_INTERRUPT_MASK_FROM_ISR( x )  ( void ) ( x )

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
