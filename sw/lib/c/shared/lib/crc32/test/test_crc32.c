#include "lib_crc32.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// [test->fw~conn_proto_002~1]
static void test_check_value(void)
{
    // The standard CRC-32 check value: crc("123456789") == 0xCBF43926.
    const uint8_t msg[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, lib_crc32_compute(msg, 9U));
}

static void test_empty_input(void)
{
    const uint8_t byte = 0U;
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, lib_crc32_compute(&byte, 0U));
}

static void test_null_input(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, lib_crc32_compute(NULL, 4U));
}

static void test_single_bit_change_changes_crc(void)
{
    const uint8_t a[4] = { 0x11U, 0x22U, 0x33U, 0x44U };
    const uint8_t b[4] = { 0x11U, 0x22U, 0x33U, 0x45U };
    TEST_ASSERT_NOT_EQUAL(lib_crc32_compute(a, 4U), lib_crc32_compute(b, 4U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_null_input);
    RUN_TEST(test_single_bit_change_changes_crc);
    return UNITY_END();
}
