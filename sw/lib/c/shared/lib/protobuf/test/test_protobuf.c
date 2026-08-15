#include "lib_protobuf.h"
#include "unity.h"
#include "pb.h"
#include <string.h>

// Test-local message descriptor, hand-authored with the same nanopb macros
// the generator emits — keeps this generic lib's suite free of any project
// schema.
typedef struct
{
    uint32_t value;
    char name[16];
} testMsg;

#define testMsg_FIELDLIST(X, a) \
    X(a, STATIC, SINGULAR, UINT32, value, 1) \
    X(a, STATIC, SINGULAR, STRING, name, 2)
#define testMsg_CALLBACK NULL
#define testMsg_DEFAULT NULL

PB_BIND(testMsg, testMsg, AUTO)

void setUp(void) {}
void tearDown(void) {}

static void test_roundtrip(void)
{
    testMsg msg = { .value = 12345U, .name = "motor" };
    uint8_t buffer[32];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(&testMsg_msg, &msg, buffer, sizeof(buffer), &encodedLen));
    TEST_ASSERT_TRUE(encodedLen > 0U);

    testMsg decoded = { 0 };
    TEST_ASSERT_TRUE(lib_protobuf_decode(&testMsg_msg, buffer, encodedLen, &decoded));
    TEST_ASSERT_EQUAL_UINT32(12345U, decoded.value);
    TEST_ASSERT_EQUAL_STRING("motor", decoded.name);
}

static void test_encode_rejects_small_buffer(void)
{
    testMsg msg = { .value = 1U, .name = "toolongforthis" };
    uint8_t buffer[4];
    size_t encodedLen = 0U;
    TEST_ASSERT_FALSE(lib_protobuf_encode(&testMsg_msg, &msg, buffer, sizeof(buffer), &encodedLen));
}

static void test_decode_rejects_truncated_bytes(void)
{
    testMsg msg = { .value = 99U, .name = "abcdef" };
    uint8_t buffer[32];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(&testMsg_msg, &msg, buffer, sizeof(buffer), &encodedLen));
    TEST_ASSERT_TRUE(encodedLen > 2U);

    testMsg decoded = { 0 };
    TEST_ASSERT_FALSE(lib_protobuf_decode(&testMsg_msg, buffer, encodedLen - 2U, &decoded));
}

static void test_null_args_rejected(void)
{
    testMsg msg = { 0 };
    uint8_t buffer[8];
    size_t encodedLen = 0U;
    TEST_ASSERT_FALSE(lib_protobuf_encode(NULL, &msg, buffer, sizeof(buffer), &encodedLen));
    TEST_ASSERT_FALSE(lib_protobuf_decode(&testMsg_msg, NULL, 4U, &msg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_encode_rejects_small_buffer);
    RUN_TEST(test_decode_rejects_truncated_bytes);
    RUN_TEST(test_null_args_rejected);
    return UNITY_END();
}
