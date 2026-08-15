#include "lib_cobs.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void assertEncodes(const uint8_t * plain, size_t plainLen,
                          const uint8_t * expected, size_t expectedLen)
{
    uint8_t enc[600];
    size_t encLen = 0U;
    TEST_ASSERT_TRUE(lib_cobs_encode(plain, plainLen, enc, sizeof(enc), &encLen));
    TEST_ASSERT_EQUAL_size_t(expectedLen, encLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, enc, expectedLen);

    uint8_t dec[600];
    size_t decLen = 0U;
    TEST_ASSERT_TRUE(lib_cobs_decode(enc, encLen, dec, sizeof(dec), &decLen));
    TEST_ASSERT_EQUAL_size_t(plainLen, decLen);
    if (plainLen > 0U)
    {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, dec, plainLen);
    }
}

// [test->fw~conn_proto_002~1]
static void test_empty_payload(void)
{
    const uint8_t plain[1] = { 0U };
    const uint8_t expected[1] = { 0x01U };
    assertEncodes(plain, 0U, expected, 1U);
}

static void test_single_zero(void)
{
    const uint8_t plain[1] = { 0x00U };
    const uint8_t expected[2] = { 0x01U, 0x01U };
    assertEncodes(plain, 1U, expected, 2U);
}

static void test_zero_zero(void)
{
    const uint8_t plain[2] = { 0x00U, 0x00U };
    const uint8_t expected[3] = { 0x01U, 0x01U, 0x01U };
    assertEncodes(plain, 2U, expected, 3U);
}

static void test_mixed_with_zero(void)
{
    const uint8_t plain[4] = { 0x11U, 0x22U, 0x00U, 0x33U };
    const uint8_t expected[5] = { 0x03U, 0x11U, 0x22U, 0x02U, 0x33U };
    assertEncodes(plain, 4U, expected, 5U);
}

static void test_no_zeros(void)
{
    const uint8_t plain[4] = { 0x11U, 0x22U, 0x33U, 0x44U };
    const uint8_t expected[5] = { 0x05U, 0x11U, 0x22U, 0x33U, 0x44U };
    assertEncodes(plain, 4U, expected, 5U);
}

static void test_trailing_zeros(void)
{
    const uint8_t plain[4] = { 0x11U, 0x00U, 0x00U, 0x00U };
    const uint8_t expected[5] = { 0x02U, 0x11U, 0x01U, 0x01U, 0x01U };
    assertEncodes(plain, 4U, expected, 5U);
}

static void test_254_nonzero_block_boundary(void)
{
    // 254 nonzero bytes: a full 0xFF block plus a trailing empty block.
    uint8_t plain[254];
    uint8_t expected[256];
    expected[0] = 0xFFU;
    for (size_t i = 0U; i < 254U; i++)
    {
        plain[i] = (uint8_t)(i + 1U);
        expected[i + 1U] = (uint8_t)(i + 1U);
    }
    expected[255] = 0x01U;
    assertEncodes(plain, 254U, expected, 256U);
}

static void test_long_roundtrip_with_scattered_zeros(void)
{
    uint8_t plain[400];
    for (size_t i = 0U; i < sizeof(plain); i++)
    {
        plain[i] = (uint8_t)((i % 7U == 0U) ? 0U : (i & 0xFFU));
    }
    uint8_t enc[600];
    size_t encLen = 0U;
    TEST_ASSERT_TRUE(lib_cobs_encode(plain, sizeof(plain), enc, sizeof(enc), &encLen));
    for (size_t i = 0U; i < encLen; i++)
    {
        TEST_ASSERT_NOT_EQUAL(0U, enc[i]);
    }
    uint8_t dec[600];
    size_t decLen = 0U;
    TEST_ASSERT_TRUE(lib_cobs_decode(enc, encLen, dec, sizeof(dec), &decLen));
    TEST_ASSERT_EQUAL_size_t(sizeof(plain), decLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, dec, sizeof(plain));
}

static void test_encode_rejects_small_output(void)
{
    const uint8_t plain[4] = { 1U, 2U, 3U, 4U };
    uint8_t enc[4];
    size_t encLen = 0U;
    TEST_ASSERT_FALSE(lib_cobs_encode(plain, 4U, enc, 4U, &encLen));
}

static void test_decode_rejects_embedded_zero(void)
{
    const uint8_t bad[2] = { 0x02U, 0x00U };
    uint8_t dec[8];
    size_t decLen = 0U;
    TEST_ASSERT_FALSE(lib_cobs_decode(bad, 2U, dec, sizeof(dec), &decLen));
}

static void test_decode_rejects_overrunning_code(void)
{
    const uint8_t bad[2] = { 0x05U, 0x11U };
    uint8_t dec[8];
    size_t decLen = 0U;
    TEST_ASSERT_FALSE(lib_cobs_decode(bad, 2U, dec, sizeof(dec), &decLen));
}

static void test_decode_rejects_empty_input(void)
{
    const uint8_t byte = 0x01U;
    uint8_t dec[8];
    size_t decLen = 0U;
    TEST_ASSERT_FALSE(lib_cobs_decode(&byte, 0U, dec, sizeof(dec), &decLen));
}

static void test_decode_rejects_output_overflow(void)
{
    const uint8_t enc[5] = { 0x05U, 0x11U, 0x22U, 0x33U, 0x44U };
    uint8_t dec[3];
    size_t decLen = 0U;
    TEST_ASSERT_FALSE(lib_cobs_decode(enc, 5U, dec, 3U, &decLen));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_payload);
    RUN_TEST(test_single_zero);
    RUN_TEST(test_zero_zero);
    RUN_TEST(test_mixed_with_zero);
    RUN_TEST(test_no_zeros);
    RUN_TEST(test_trailing_zeros);
    RUN_TEST(test_254_nonzero_block_boundary);
    RUN_TEST(test_long_roundtrip_with_scattered_zeros);
    RUN_TEST(test_encode_rejects_small_output);
    RUN_TEST(test_decode_rejects_embedded_zero);
    RUN_TEST(test_decode_rejects_overrunning_code);
    RUN_TEST(test_decode_rejects_empty_input);
    RUN_TEST(test_decode_rejects_output_overflow);
    return UNITY_END();
}
