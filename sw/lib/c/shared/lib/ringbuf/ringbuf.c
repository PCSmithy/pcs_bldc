#include "ringbuf.h"

void ringbuf_init(ringbuf_t *rb, uint8_t *storage, size_t capacity) {
    rb->buffer   = storage;
    rb->capacity = capacity;
    rb->head     = 0;
    rb->tail     = 0;
    rb->count    = 0;
}

bool ringbuf_push(ringbuf_t *rb, uint8_t byte) {
    if (rb->count >= rb->capacity) {
        return false;
    }
    rb->buffer[rb->head] = byte;
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count++;
    return true;
}

bool ringbuf_pop(ringbuf_t *rb, uint8_t *out) {
    if (rb->count == 0) {
        return false;
    }
    *out = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count--;
    return true;
}

size_t ringbuf_count(const ringbuf_t *rb) {
    return rb->count;
}

size_t ringbuf_capacity(const ringbuf_t *rb) {
    return rb->capacity;
}
