/*
 * Coroutine backend for the native cooperative FreeRTOS port (port_common.c).
 *
 * The non-Windows counterpart of Native-Fiber: identical cooperative semantics,
 * but the switch is a hand-rolled asm coroutine (coro.*) rather than a Win32
 * fiber, so it needs no OS fiber API and no deprecated ucontext. Nothing is
 * claimed from the OS — the main coroutine is the host thread's own context —
 * so a second firmware image on the thread simply bootstraps its own.
 */
#include <stdlib.h>

#include "FreeRTOS.h"
#include "coro.h"
#include "port_backend.h"

static coro_t   mainCoro;
/* &mainCoro while the scheduler is running, NULL before start and after
 * teardown — the backend's "live" flag as well as the switch-back target. */
static coro_t * pxMainCoro = NULL;

void vPortBackendCreateContext( ThreadState_t * const ts )
{
    /* One allocation per task: the coro_t sits at the head of the block and the
     * rest is its stack, so freeing ts->pvContext releases both. */
    void * const pvBlock = malloc( sizeof( coro_t ) + PORT_TASK_STACK_BYTES );
    configASSERT( pvBlock != NULL );

    coro_t * const c = ( coro_t * ) pvBlock;
    coro_init( c, ( void * ) ( c + 1 ), PORT_TASK_STACK_BYTES,
               vPortCommonTaskEntry, ts );
    ts->pvContext = c;
}

void vPortBackendDestroyContext( ThreadState_t * const ts )
{
    free( ts->pvContext );
    ts->pvContext = NULL;
}

void vPortBackendStart( void )
{
    /* Bind the host thread's own context as the main coroutine. Re-bootstrapping
     * on every start is what makes the port restartable. */
    coro_bootstrap( &mainCoro );
    pxMainCoro = &mainCoro;
}

void vPortBackendEnd( void )
{
    pxMainCoro = NULL;
}

void vPortBackendSwitchTo( ThreadState_t * const ts )
{
    coro_switch( ( coro_t * ) ts->pvContext );
}

void vPortBackendSwitchToDriver( void )
{
    coro_switch( pxMainCoro );
}

BaseType_t xPortBackendIsLive( void )
{
    return ( pxMainCoro != NULL ) ? pdTRUE : pdFALSE;
}

BaseType_t xPortBackendOnDriver( void )
{
    /* coro_current() keeps returning the last-switched-to coroutine after
     * teardown, so answer from pxMainCoro alone before start / after teardown. */
    return ( ( pxMainCoro == NULL ) || ( coro_current() == pxMainCoro ) ) ? pdTRUE : pdFALSE;
}
