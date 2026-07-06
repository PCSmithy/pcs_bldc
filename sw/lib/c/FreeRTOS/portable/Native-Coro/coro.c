#include "coro.h"
#include <stdint.h>

/* Assembly backend (coro_switch_<arch>.S). */
extern void coro_asm_swap( void ** saveSp, void * newSp );
extern void coro_asm_trampoline( void );

#if defined( __aarch64__ )

/* Saved-frame layout — must match coro_switch_aarch64.S (x19-x30, d8-d15). */
#define CORO_FRAME_BYTES  160u
#define CORO_OFF_ENTRY    0u    /* x19 */
#define CORO_OFF_ARG      8u    /* x20 */
#define CORO_OFF_RET      88u   /* x30 (link register) */

#elif defined( __x86_64__ )

/* Saved-frame layout — must match coro_switch_x86_64.S (rbp, rbx, r12-r15,
 * then the return address). FRAME_BYTES is 56 so an aligned stack top leaves
 * the trampoline's `call` 16-byte aligned per the System V ABI. */
#define CORO_FRAME_BYTES  56u
#define CORO_OFF_ENTRY    24u   /* r12 */
#define CORO_OFF_ARG      16u   /* r13 */
#define CORO_OFF_RET      48u   /* return address */

#else
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
