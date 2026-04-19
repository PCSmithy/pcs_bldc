#ifndef PCS_LIB_RINGBUF_H
#define PCS_LIB_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Channelizable byte ring buffer.
//
// Each ringbuf_t is a channel; the caller owns the backing storage and
// passes it to ringbuf_init. There is no internal allocation, so this is
// usable from any context (ISR included, with caller-managed sync).

typedef struct {
    uint8_t *buffer;
    size_t   capacity;
    size_t   head;
    size_t   tail;
    size_t   count;
} ringbuf_t;

void   ringbuf_init    (ringbuf_t *rb, uint8_t *storage, size_t capacity);
bool   ringbuf_push    (ringbuf_t *rb, uint8_t byte);
bool   ringbuf_pop     (ringbuf_t *rb, uint8_t *out);
size_t ringbuf_count   (const ringbuf_t *rb);
size_t ringbuf_capacity(const ringbuf_t *rb);

#endif
