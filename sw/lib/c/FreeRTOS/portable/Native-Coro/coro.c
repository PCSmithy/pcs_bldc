#include "coro.h"
#include "coro_frame.h"   /* CORO_FRAME_BYTES / CORO_OFF_* — shared with the .S */
#include <stdint.h>

/* Assembly backend (coro_switch_<arch>.S). */
extern void coro_asm_swap( void ** saveSp, void * newSp );
extern void coro_asm_trampoline( void );

#if !defined( CORO_FRAME_BYTES )
#error "Native-Coro: no context-switch backend for this architecture yet"
#endif

/* The frame geometry lives in coro_frame.h so the C that builds a fresh frame
 * and the asm that saves/restores it share one source of truth. Guard the
 * invariants the asm relies on at compile time (these also protect x86_64):
 *  - 64-bit pointers (each saved slot is one register width);
 *  - the three coro_init slots are 8-byte aligned and inside the frame;
 *  - AArch64 keeps the frame 16-aligned; x86-64 keeps it 8 mod 16 by design
 *    (56 bytes) so the trampoline's `call` lands 16-aligned per System V. */
_Static_assert( sizeof( void * ) == 8, "coro frame assumes 64-bit pointers" );
_Static_assert( ( CORO_OFF_ENTRY % 8 ) == 0, "entry slot must be 8-aligned" );
_Static_assert( ( CORO_OFF_ARG % 8 ) == 0, "arg slot must be 8-aligned" );
_Static_assert( ( CORO_OFF_RET % 8 ) == 0, "return slot must be 8-aligned" );
_Static_assert( CORO_OFF_ENTRY < CORO_FRAME_BYTES, "entry slot outside frame" );
_Static_assert( CORO_OFF_ARG < CORO_FRAME_BYTES, "arg slot outside frame" );
_Static_assert( CORO_OFF_RET < CORO_FRAME_BYTES, "return slot outside frame" );
#if defined( __aarch64__ )
_Static_assert( ( CORO_FRAME_BYTES % 16 ) == 0, "AArch64 frame must be 16-aligned" );
#elif defined( __x86_64__ )
_Static_assert( ( CORO_FRAME_BYTES % 16 ) == 8, "x86-64 frame must be 8 mod 16 for call alignment" );
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

coro_t * coro_current( void )
{
    return coro_running;
}

void coro_switch( coro_t * to )
{
    coro_t * from = coro_running;
    coro_running = to;
    coro_asm_swap( &from->sp, to->sp );
}
