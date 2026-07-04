#include "coro.h"
#include <stdint.h>

/* Assembly backend (coro_switch_<arch>.S). */
extern void coro_asm_swap( void ** saveSp, void * newSp );
extern void coro_asm_trampoline( void );

#if defined( __aarch64__ )

/* Saved-frame layout — must match coro_switch_aarch64.S. */
#define CORO_FRAME_BYTES  160u
#define CORO_OFF_ENTRY    0u    /* x19 */
#define CORO_OFF_ARG      8u    /* x20 */
#define CORO_OFF_RET      88u   /* x30 (link register) */

#else
/* Future: add an x86-64 backend for Linux/Intel hosts — a coro_switch_x86_64.S
 * saving the System V callee-saved set (rbx, rbp, r12-r15) with the matching
 * frame offsets below, then drop the arch guard in FreeRTOS/CMakeLists.txt.
 * Deferred until a non-arm64 host actually needs the native/SIL build. */
#error "Native-Coro: no context-switch backend for this architecture yet"
#endif

static coro_t * coro_running;

void coro_bootstrap( coro_t * mainCoro )
{
    coro_running = mainCoro;
}

void coro_init( coro_t * c, void * stackMem, size_t stackBytes,
                void ( * entry )( void * ), void * arg )
{
    uintptr_t top   = ( uintptr_t ) stackMem + stackBytes;
    top   &= ~( uintptr_t ) 15u;                 /* 16-byte align the stack top */
    uintptr_t frame = top - CORO_FRAME_BYTES;

    uint8_t * f = ( uint8_t * ) frame;
    for( size_t i = 0u; i < CORO_FRAME_BYTES; i++ )
    {
        f[ i ] = 0u;
    }
    /* First switch in restores these slots, then `ret`s into the trampoline,
     * which calls entry(arg) with the frame consumed and sp at the stack top. */
    *( void ** ) ( f + CORO_OFF_ENTRY ) = ( void * ) entry;
    *( void ** ) ( f + CORO_OFF_ARG )   = arg;
    *( void ** ) ( f + CORO_OFF_RET )   = ( void * ) coro_asm_trampoline;

    c->sp = ( void * ) frame;
}

void coro_switch( coro_t * to )
{
    coro_t * from = coro_running;
    coro_running = to;
    coro_asm_swap( &from->sp, to->sp );
}
