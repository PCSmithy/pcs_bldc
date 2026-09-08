#include "HW_I2C.h"
#include "HW_I2C_timeout.h"
#include "unity.h"

// A 7-bit device address on BUS_1 (CYPD3177 HPI lives at 0x08). Behavioral
// transfer coverage (round-trips, faults) lives in the SIL suite
// (sw/sil/pcs_bldc_sil/tests/i2c_behavior.rs), driven through firmware traffic.
#define DEV_A    (0x08U)

// File-scope config the tests build (good baseline) and tweak per case.
// HW_I2C_init stores a pointer to it, so it must outlive each test.
static HW_I2C_busConfig_S i2cBuses[HW_I2C_BUS_COUNT];
static HW_I2C_config_S    i2cConfig;

static void buildGoodConfig(void)
{
    i2cBuses[HW_I2C_BUS_1] = (HW_I2C_busConfig_S){
        .enabled = true, .transferMode = HW_I2C_TRANSFERMODE_INTERRUPT,
        .sclBitRateHz = 100000U, .busNameStr = "I2C1" };
    i2cBuses[HW_I2C_BUS_2] = (HW_I2C_busConfig_S){ .enabled = false };

    i2cConfig = (HW_I2C_config_S){ .buses = i2cBuses, .numBuses = HW_I2C_BUS_COUNT };
}

void setUp(void)
{
    // A rejected init is the clean slate: init drops the driver to its
    // uninitialized state before it looks at the config.
    (void)HW_I2C_init(NULL);
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- fw~hal_i2c_001: init + config validation ---- */
// [test->fw~hal_i2c_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));
}

// [test->fw~hal_i2c_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_I2C_init(NULL));
}

// [test->fw~hal_i2c_001~1]
static void test_init_unsupported_transfer_mode(void)
{
    i2cBuses[HW_I2C_BUS_1].transferMode = HW_I2C_TRANSFERMODE_DMA; // reserved, unimplemented
    TEST_ASSERT_FALSE(HW_I2C_init(&i2cConfig));
}

// [test->fw~hal_i2c_001~1]
static void test_init_disabled_bus_ignores_mode(void)
{
    i2cBuses[HW_I2C_BUS_1].enabled = false;
    i2cBuses[HW_I2C_BUS_1].transferMode = HW_I2C_TRANSFERMODE_DMA; // ignored while disabled
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));
}

/* ---- fw~hal_i2c_003 / _004: transfer guards ---- */
// [test->fw~hal_i2c_003~1]
// [test->fw~hal_i2c_004~1]
static void test_transfers_before_init_fail(void)
{
    uint8_t buf[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_A, buf, 2U));
    TEST_ASSERT_FALSE(HW_I2C_receive(HW_I2C_BUS_1, DEV_A, buf, 2U));
    TEST_ASSERT_FALSE(HW_I2C_memRead(HW_I2C_BUS_1, DEV_A, 0x00U, HW_I2C_MEMADDR_SIZE_8BIT, buf, 2U));
    TEST_ASSERT_FALSE(HW_I2C_memWrite(HW_I2C_BUS_1, DEV_A, 0x00U, HW_I2C_MEMADDR_SIZE_8BIT, buf, 2U));
}

// [test->fw~hal_i2c_003~1]
// [test->fw~hal_i2c_004~1]
static void test_transfer_rejects_bad_arguments(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    uint8_t buf[2] = { 0U, 0U };
    // Zero length, NULL data, a disabled bus, and an out-of-range bus all fail.
    TEST_ASSERT_FALSE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_A, buf, 0U));
    TEST_ASSERT_FALSE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_A, NULL, 2U));
    TEST_ASSERT_FALSE(HW_I2C_memRead(HW_I2C_BUS_2, DEV_A, 0x00U, HW_I2C_MEMADDR_SIZE_8BIT, buf, 2U));
    TEST_ASSERT_FALSE(HW_I2C_receive(HW_I2C_BUS_COUNT, DEV_A, buf, 2U));
}

/* ---- fw~hal_i2c_003: computed timeout ---- */
// [test->fw~hal_i2c_003~1]
static void test_timeout_formula(void)
{
    // ceil(9900*N / f_bit) + 1ms.
    TEST_ASSERT_EQUAL_UINT32(2U,   HW_I2C_computeTimeoutMs(100000U, 1U));    // 0.099 -> 1, +1
    TEST_ASSERT_EQUAL_UINT32(2U,   HW_I2C_computeTimeoutMs(100000U, 10U));   // 0.99  -> 1, +1
    TEST_ASSERT_EQUAL_UINT32(11U,  HW_I2C_computeTimeoutMs(100000U, 100U));  // 9.9   -> 10, +1
    TEST_ASSERT_EQUAL_UINT32(100U, HW_I2C_computeTimeoutMs(100000U, 1000U)); // 99    -> 99, +1
    TEST_ASSERT_EQUAL_UINT32(2U,   HW_I2C_computeTimeoutMs(400000U, 10U));   // 0.2475 -> 1, +1
    TEST_ASSERT_EQUAL_UINT32(0U,   HW_I2C_computeTimeoutMs(0U, 100U));       // no bit rate
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_unsupported_transfer_mode);
    RUN_TEST(test_init_disabled_bus_ignores_mode);

    RUN_TEST(test_transfers_before_init_fail);
    RUN_TEST(test_transfer_rejects_bad_arguments);

    RUN_TEST(test_timeout_formula);

    return UNITY_END();
}
