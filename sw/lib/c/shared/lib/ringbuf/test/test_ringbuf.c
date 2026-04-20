#include "ringbuf.h"
#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_init_empty(void) {
    uint8_t storage[8];
    ringbuf_t rb;
    ringbuf_init(&rb, storage, sizeof(storage));
    TEST_ASSERT_EQUAL_INT(0, ringbuf_count(&rb));
    TEST_ASSERT_EQUAL_INT(8, ringbuf_capacity(&rb));
}

static void test_push_pop_roundtrip(void) {
    uint8_t storage[8];
    ringbuf_t rb;
    ringbuf_init(&rb, storage, sizeof(storage));
    TEST_ASSERT_TRUE(ringbuf_push(&rb, 42));
    TEST_ASSERT_EQUAL_INT(1, ringbuf_count(&rb));
    uint8_t out;
    TEST_ASSERT_TRUE(ringbuf_pop(&rb, &out));
    TEST_ASSERT_EQUAL_UINT8(42, out);
    TEST_ASSERT_EQUAL_INT(0, ringbuf_count(&rb));
}

static void test_push_until_full_then_fail(void) {
    uint8_t storage[4];
    ringbuf_t rb;
    ringbuf_init(&rb, storage, sizeof(storage));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(ringbuf_push(&rb, (uint8_t)i));
    }
    TEST_ASSERT_FALSE(ringbuf_push(&rb, 99));
    TEST_ASSERT_EQUAL_INT(4, ringbuf_count(&rb));
}

static void test_pop_when_empty_fails(void) {
    uint8_t storage[4];
    ringbuf_t rb;
    ringbuf_init(&rb, storage, sizeof(storage));
    uint8_t out;
    TEST_ASSERT_FALSE(ringbuf_pop(&rb, &out));
}

static void test_wraparound(void) {
    uint8_t storage[4];
    ringbuf_t rb;
    ringbuf_init(&rb, storage, sizeof(storage));
    for (int i = 0; i < 4; i++) ringbuf_push(&rb, (uint8_t)i);
    uint8_t out;
    for (int i = 0; i < 3; i++) ringbuf_pop(&rb, &out);
    for (int i = 100; i < 103; i++) ringbuf_push(&rb, (uint8_t)i);
    TEST_ASSERT_EQUAL_INT(4, ringbuf_count(&rb));
    ringbuf_pop(&rb, &out);
    TEST_ASSERT_EQUAL_UINT8(3, out);
    for (int expected = 100; expected < 103; expected++) {
        TEST_ASSERT_TRUE(ringbuf_pop(&rb, &out));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)expected, out);
    }
    TEST_ASSERT_EQUAL_INT(0, ringbuf_count(&rb));
}

static void test_two_independent_channels(void) {
    // Channelization: two ringbufs share zero state.
    uint8_t storage_a[4], storage_b[4];
    ringbuf_t a, b;
    ringbuf_init(&a, storage_a, sizeof(storage_a));
    ringbuf_init(&b, storage_b, sizeof(storage_b));
    ringbuf_push(&a, 1);
    ringbuf_push(&a, 2);
    ringbuf_push(&b, 99);
    TEST_ASSERT_EQUAL_INT(2, ringbuf_count(&a));
    TEST_ASSERT_EQUAL_INT(1, ringbuf_count(&b));
    uint8_t out;
    ringbuf_pop(&b, &out);
    TEST_ASSERT_EQUAL_UINT8(99, out);
    TEST_ASSERT_EQUAL_INT(2, ringbuf_count(&a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_empty);
    RUN_TEST(test_push_pop_roundtrip);
    RUN_TEST(test_push_until_full_then_fail);
    RUN_TEST(test_pop_when_empty_fails);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_two_independent_channels);
    return UNITY_END();
}
