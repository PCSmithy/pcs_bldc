/*
 * D1 spike — driver + two-task program for the cooperative fiber port.
 *
 *   trace mode (default):  run TRACE_TICKS ticks, dump the per-tick task
 *                          activity to stdout. Diff across runs / parallel
 *                          launches → must be bit-identical (determinism).
 *   bench mode ("bench N"): run N ticks, print ticks/s + x-realtime to
 *                          stderr (stdout stays clean). (performance.)
 *
 * Two tasks at different priority + rate exercise the scheduler:
 *   A — prio 3, 1 ms period
 *   B — prio 2, 5 ms period
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

/* Port entry points used by the driver / idle hook. */
extern void vSilAdvanceTick( void );
extern void vPortYieldToScheduler( void );

#define TRACE_TICKS    50

/* Per-activation event log (tick, task). */
typedef struct { uint32_t tick; char task; } Event_t;
static Event_t g_log[ 1u << 20 ];
static size_t  g_logCount = 0u;
static int     g_trace    = 1;   /* record events only in trace mode */

static void logRun( char task )
{
    if( g_trace && ( g_logCount < ( sizeof( g_log ) / sizeof( g_log[ 0 ] ) ) ) )
    {
        g_log[ g_logCount ].tick = ( uint32_t ) xTaskGetTickCount();
        g_log[ g_logCount ].task = task;
        g_logCount++;
    }
}

static void taskA( void * pv )
{
    ( void ) pv;
    TickType_t last = xTaskGetTickCount();
    for( ;; )
    {
        vTaskDelayUntil( &last, pdMS_TO_TICKS( 1 ) );
        logRun( 'A' );
    }
}

static void taskB( void * pv )
{
    ( void ) pv;
    TickType_t last = xTaskGetTickCount();
    for( ;; )
    {
        vTaskDelayUntil( &last, pdMS_TO_TICKS( 5 ) );
        logRun( 'B' );
    }
}

/* Idle hook = quiescence handoff back to the driver (main fiber). */
void vApplicationIdleHook( void )
{
    vPortYieldToScheduler();
}

int main( int argc, char ** argv )
{
    long long nTicks = TRACE_TICKS;
    int bench = 0;

    if( ( argc >= 2 ) && ( strcmp( argv[ 1 ], "bench" ) == 0 ) )
    {
        bench   = 1;
        g_trace = 0;
        nTicks  = ( argc >= 3 ) ? atoll( argv[ 2 ] ) : 10000000LL;
    }

    xTaskCreate( taskA, "A", configMINIMAL_STACK_SIZE, NULL, 3, NULL );
    xTaskCreate( taskB, "B", configMINIMAL_STACK_SIZE, NULL, 2, NULL );

    /* Runs the firmware to first quiescence, then returns (fiber port). */
    vTaskStartScheduler();

    clock_t t0 = clock();
    for( long long i = 0; i < nTicks; i++ )
    {
        vSilAdvanceTick();
    }
    clock_t t1 = clock();

    if( bench )
    {
        double secs = ( double ) ( t1 - t0 ) / ( double ) CLOCKS_PER_SEC;
        double tps  = ( secs > 0.0 ) ? ( ( double ) nTicks / secs ) : 0.0;
        fprintf( stderr, "bench: %lld ticks in %.3f s = %.2f Mticks/s, %.0fx realtime\n",
                 nTicks, secs, tps / 1e6, tps / ( double ) configTICK_RATE_HZ );
    }
    else
    {
        for( size_t i = 0; i < g_logCount; i++ )
        {
            printf( "%u %c\n", g_log[ i ].tick, g_log[ i ].task );
        }
    }

    return 0;
}
