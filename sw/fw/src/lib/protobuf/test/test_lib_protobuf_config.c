#include "lib_protobuf_config.h"
#include "lib_protobuf.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// [test->fw~conn_proto_001~1]
static void test_ping_roundtrip(void)
{
    pcs_Envelope env = pcs_Envelope_init_zero;
    env.request_id = 7U;
    env.which_payload = pcs_Envelope_ping_tag;

    uint8_t buffer[LIB_PROTOBUF_ENVELOPE_MAX];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(pcs_Envelope_fields, &env, buffer, sizeof(buffer), &encodedLen));

    pcs_Envelope decoded = pcs_Envelope_init_zero;
    TEST_ASSERT_TRUE(lib_protobuf_decode(pcs_Envelope_fields, buffer, encodedLen, &decoded));
    TEST_ASSERT_EQUAL_UINT32(7U, decoded.request_id);
    TEST_ASSERT_EQUAL(pcs_Envelope_ping_tag, decoded.which_payload);
}

// [test->fw~conn_proto_001~1]
static void test_response_roundtrip(void)
{
    pcs_Envelope env = pcs_Envelope_init_zero;
    env.request_id = 42U;
    env.which_payload = pcs_Envelope_response_tag;
    env.payload.response.accepted = false;
    (void)strcpy(env.payload.response.cause, "unknown mode");

    uint8_t buffer[LIB_PROTOBUF_ENVELOPE_MAX];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(pcs_Envelope_fields, &env, buffer, sizeof(buffer), &encodedLen));

    pcs_Envelope decoded = pcs_Envelope_init_zero;
    TEST_ASSERT_TRUE(lib_protobuf_decode(pcs_Envelope_fields, buffer, encodedLen, &decoded));
    TEST_ASSERT_EQUAL_UINT32(42U, decoded.request_id);
    TEST_ASSERT_EQUAL(pcs_Envelope_response_tag, decoded.which_payload);
    TEST_ASSERT_FALSE(decoded.payload.response.accepted);
    TEST_ASSERT_EQUAL_STRING("unknown mode", decoded.payload.response.cause);
}

// [test->fw~conn_proto_001~1]
static void test_log_roundtrip(void)
{
    pcs_Envelope env = pcs_Envelope_init_zero;
    env.which_payload = pcs_Envelope_log_tag;
    (void)strcpy(env.payload.log.text, "hello from the board");

    uint8_t buffer[LIB_PROTOBUF_ENVELOPE_MAX];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(pcs_Envelope_fields, &env, buffer, sizeof(buffer), &encodedLen));

    pcs_Envelope decoded = pcs_Envelope_init_zero;
    TEST_ASSERT_TRUE(lib_protobuf_decode(pcs_Envelope_fields, buffer, encodedLen, &decoded));
    TEST_ASSERT_EQUAL_UINT32(0U, decoded.request_id);
    TEST_ASSERT_EQUAL(pcs_Envelope_log_tag, decoded.which_payload);
    TEST_ASSERT_EQUAL_STRING("hello from the board", decoded.payload.log.text);
}

// [test->fw~conn_proto_001~1]
static void test_truncated_encoding_fails_decode(void)
{
    pcs_Envelope env = pcs_Envelope_init_zero;
    env.which_payload = pcs_Envelope_log_tag;
    (void)strcpy(env.payload.log.text, "truncate me");

    uint8_t buffer[LIB_PROTOBUF_ENVELOPE_MAX];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(pcs_Envelope_fields, &env, buffer, sizeof(buffer), &encodedLen));
    TEST_ASSERT_TRUE(encodedLen > 2U);

    pcs_Envelope decoded = pcs_Envelope_init_zero;
    TEST_ASSERT_FALSE(lib_protobuf_decode(pcs_Envelope_fields, buffer, encodedLen - 2U, &decoded));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ping_roundtrip);
    RUN_TEST(test_response_roundtrip);
    RUN_TEST(test_log_roundtrip);
    RUN_TEST(test_truncated_encoding_fails_decode);
    return UNITY_END();
}
