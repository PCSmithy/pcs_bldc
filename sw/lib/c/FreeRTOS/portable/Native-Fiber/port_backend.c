/*
 * Win32-fiber backend for the native cooperative FreeRTOS port (port_common.c).
 *
 * Each task is a fiber; the driver ("framework") is the fiber the host thread
 * was converted to. The port converts the thread only when it is not already a
 * fiber, so a second firmware image sharing the thread borrows that conversion
 * and leaves the un-convert to whichever image owns it.
 */
#include <windows.h>

#include "FreeRTOS.h"
#include "port_backend.h"

static void *     pvMainFiber     = NULL;
/* pdTRUE when this port instance converted the thread (so teardown un-converts
 * it); pdFALSE when it borrowed a conversion another image made. */
static BaseType_t xOwnsConversion = pdFALSE;

static void CALLBACK prvFiberEntry( void * lpParam )
{
    vPortCommonTaskEntry( lpParam );
}

void vPortBackendCreateContext( ThreadState_t * const ts )
{
    ts->pvContext = CreateFiber( PORT_TASK_STACK_BYTES, prvFiberEntry, ts );
    configASSERT( ts->pvContext != NULL );
}

void vPortBackendDestroyContext( ThreadState_t * const ts )
{
    if( ts->pvContext != NULL )
    {
        DeleteFiber( ts->pvContext );
        ts->pvContext = NULL;
    }
}

void vPortBackendStart( void )
{
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
}

void vPortBackendEnd( void )
{
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

void vPortBackendSwitchTo( ThreadState_t * const ts )
{
    SwitchToFiber( ts->pvContext );
}

void vPortBackendSwitchToDriver( void )
{
    SwitchToFiber( pvMainFiber );
}

BaseType_t xPortBackendIsLive( void )
{
    return ( pvMainFiber != NULL ) ? pdTRUE : pdFALSE;
}

BaseType_t xPortBackendOnDriver( void )
{
    /* GetCurrentFiber() on a thread that was never converted reads an undefined
     * TEB field, so answer from pvMainFiber alone before start / after teardown. */
    return ( ( pvMainFiber == NULL ) || ( GetCurrentFiber() == pvMainFiber ) ) ? pdTRUE : pdFALSE;
}
