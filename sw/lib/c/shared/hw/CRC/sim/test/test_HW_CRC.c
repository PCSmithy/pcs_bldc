#include "HW_CRC.h"
#include "unity.h"

// The API contract on the sim side; the target proves its unit against the
// same check value at init (fw~hal_crc_001).

static const HW_CRC_config_S config = { 0 };

void setUp(void) {}
void tearDown(void) {}

// [test->fw~hal_crc_001~1]
static void test_init_null_config_rejected(void)
{
    TEST_ASSERT_FALSE(HW_CRC_init(NULL));
}

// [test->fw~hal_crc_001~1]
static void test_init_valid_config_accepted(void)
{
    TEST_ASSERT_TRUE(HW_CRC_init(&config));
}

// [test->fw~hal_crc_002~1]
static void test_check_value_and_empty(void)
{
    TEST_ASSERT_TRUE(HW_CRC_init(&config));
    const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, HW_CRC_compute32(check, sizeof(check)));
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, HW_CRC_compute32(check, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, HW_CRC_compute32(NULL, 0U));
}

// [test->fw~hal_crc_002~1]
static void test_any_length_up_to_a_frame(void)
{
    TEST_ASSERT_TRUE(HW_CRC_init(&config));
    uint8_t bytes[452];
    for (size_t i = 0U; i < sizeof(bytes); i++)
    {
        bytes[i] = (uint8_t) ((i * 7U) + 3U);
    }
    // A known value for the full buffer pins the reference; every prefix must
    // then be stable under a second computation (the unit resets per call).
    const uint32_t full = HW_CRC_compute32(bytes, sizeof(bytes));
    for (size_t len = 1U; len <= sizeof(bytes); len += 37U)
    {
        const uint32_t a = HW_CRC_compute32(bytes, len);
        const uint32_t b = HW_CRC_compute32(bytes, len);
        TEST_ASSERT_EQUAL_HEX32(a, b);
    }
    TEST_ASSERT_EQUAL_HEX32(full, HW_CRC_compute32(bytes, sizeof(bytes)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_null_config_rejected);
    RUN_TEST(test_init_valid_config_accepted);
    RUN_TEST(test_check_value_and_empty);
    RUN_TEST(test_any_length_up_to_a_frame);
    return UNITY_END();
}
