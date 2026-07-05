#include "HW_I2C.h"
#include "HW_I2C_sim.h"
#include "HW_I2C_timeout.h"
#include "unity.h"

// Two 7-bit device addresses sharing BUS_1 (CYPD3177 HPI lives at 0x08).
#define DEV_A    (0x08U)
#define DEV_B    (0x42U)

// File-scope config the tests build (good baseline) and tweak per case.
// HW_I2C_init stores a pointer to it, so it must outlive each test.
static HW_I2C_busConfig_S i2cBuses[HW_I2C_BUS_COUNT];
static HW_I2C_config_S    i2cConfig;

static void buildGoodConfig(void)
{
    i2cBuses[HW_I2C_BUS_1] = (HW_I2C_busConfig_S){
        .enabled = true, .transferMode = HW_I2C_TRANSFERMODE_INTERRUPT,
        .sclBitRateHz = 100000U, .busNameStr = "I2C1" };

    i2cConfig = (HW_I2C_config_S){ .buses = i2cBuses, .numBuses = HW_I2C_BUS_COUNT };
}

void setUp(void)
{
    HW_I2C_sim_reset();
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

/* ---- fw~hal_i2c_002: per-transfer device addressing over a shared bus ---- */
// [test->fw~hal_i2c_002~1]
static void test_two_devices_transmit_independently(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    uint8_t txA[3] = { 0xA0U, 0xA1U, 0xA2U };
    uint8_t txB[2] = { 0xB0U, 0xB1U };
    TEST_ASSERT_TRUE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_A, txA, 3U));
    TEST_ASSERT_TRUE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_B, txB, 2U));

    uint8_t capA[3] = { 0U };
    uint8_t capB[2] = { 0U };
    TEST_ASSERT_EQUAL_UINT(3U, HW_I2C_sim_getLastTx(HW_I2C_BUS_1, DEV_A, capA, 3U));
    TEST_ASSERT_EQUAL_UINT(2U, HW_I2C_sim_getLastTx(HW_I2C_BUS_1, DEV_B, capB, 2U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(txA, capA, 3U);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(txB, capB, 2U);
}

// [test->fw~hal_i2c_002~1]
static void test_two_devices_receive_independently(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    uint8_t injA[2] = { 0x11U, 0x22U };
    uint8_t injB[2] = { 0x33U, 0x44U };
    HW_I2C_sim_setInjectedRx(HW_I2C_BUS_1, DEV_A, injA, 2U);
    HW_I2C_sim_setInjectedRx(HW_I2C_BUS_1, DEV_B, injB, 2U);

    uint8_t rxA[2] = { 0U, 0U };
    uint8_t rxB[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_I2C_receive(HW_I2C_BUS_1, DEV_A, rxA, 2U));
    TEST_ASSERT_TRUE(HW_I2C_receive(HW_I2C_BUS_1, DEV_B, rxB, 2U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(injA, rxA, 2U);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(injB, rxB, 2U);
}

/* ---- fw~hal_i2c_003: blocking byte transfers + computed timeout ---- */
// [test->fw~hal_i2c_003~1]
static void test_transmit_moves_exact_length(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    uint8_t tx[4] = { 0x10U, 0x20U, 0x30U, 0x40U };
    TEST_ASSERT_TRUE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_A, tx, 4U));

    uint8_t captured[4] = { 0U };
    const size_t n = HW_I2C_sim_getLastTx(HW_I2C_BUS_1, DEV_A, captured, 4U);
    TEST_ASSERT_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, captured, 4U);
}

// [test->fw~hal_i2c_003~1]
static void test_receive_returns_injected_bytes(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    uint8_t injected[3] = { 0xDEU, 0xADU, 0xBEU };
    HW_I2C_sim_setInjectedRx(HW_I2C_BUS_1, DEV_A, injected, 3U);

    uint8_t rx[3] = { 0U, 0U, 0U };
    TEST_ASSERT_TRUE(HW_I2C_receive(HW_I2C_BUS_1, DEV_A, rx, 3U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(injected, rx, 3U);
}

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

// [test->fw~hal_i2c_003~1]
static void test_transfer_timeout_returns_false(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    HW_I2C_sim_setStall(HW_I2C_BUS_1, true);
    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_I2C_transmit(HW_I2C_BUS_1, DEV_A, tx, 2U));
}

// [test->fw~hal_i2c_003~1]
static void test_transfer_nack_returns_false(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    HW_I2C_sim_setForceError(HW_I2C_BUS_1, true);
    uint8_t rx[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_I2C_receive(HW_I2C_BUS_1, DEV_A, rx, 2U));
}

/* ---- fw~hal_i2c_004: register read/write with 8-bit and 16-bit offsets ---- */
// [test->fw~hal_i2c_004~1]
static void test_mem_write_read_8bit_offset(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    const uint16_t reg = 0x10U;
    uint8_t wr[4] = { 0x01U, 0x02U, 0x03U, 0x04U };
    TEST_ASSERT_TRUE(HW_I2C_memWrite(HW_I2C_BUS_1, DEV_A, reg, HW_I2C_MEMADDR_SIZE_8BIT, wr, 4U));

    uint8_t rd[4] = { 0U };
    TEST_ASSERT_TRUE(HW_I2C_memRead(HW_I2C_BUS_1, DEV_A, reg, HW_I2C_MEMADDR_SIZE_8BIT, rd, 4U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(wr, rd, 4U);
}

// [test->fw~hal_i2c_004~1]
static void test_mem_write_read_16bit_offset(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    const uint16_t reg = 0x0100U;
    uint8_t wr[3] = { 0xAAU, 0xBBU, 0xCCU };
    TEST_ASSERT_TRUE(HW_I2C_memWrite(HW_I2C_BUS_1, DEV_B, reg, HW_I2C_MEMADDR_SIZE_16BIT, wr, 3U));

    uint8_t rd[3] = { 0U };
    TEST_ASSERT_TRUE(HW_I2C_memRead(HW_I2C_BUS_1, DEV_B, reg, HW_I2C_MEMADDR_SIZE_16BIT, rd, 3U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(wr, rd, 3U);
}

// [test->fw~hal_i2c_004~1]
// The CYPD3177 HPI registers live at 0x1008..0x1017 — the register space must
// reach them or every PD read silently fails on the native target.
static void test_mem_write_read_cypd3177_hpi_range(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    const uint16_t reg = 0x1014U;   // CURRENT_RDO, highest fetched offset
    uint8_t wr[4] = { 0x11U, 0x22U, 0x33U, 0x44U };
    TEST_ASSERT_TRUE(HW_I2C_memWrite(HW_I2C_BUS_1, DEV_B, reg, HW_I2C_MEMADDR_SIZE_16BIT_LSBFIRST, wr, 4U));

    uint8_t rd[4] = { 0U };
    TEST_ASSERT_TRUE(HW_I2C_memRead(HW_I2C_BUS_1, DEV_B, reg, HW_I2C_MEMADDR_SIZE_16BIT_LSBFIRST, rd, 4U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(wr, rd, 4U);
}

// [test->fw~hal_i2c_004~1]
static void test_mem_read_returns_seeded_register_bytes(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    const uint16_t reg = 0x20U;
    uint8_t seed[2] = { 0x5AU, 0xA5U };
    HW_I2C_sim_setRegBytes(HW_I2C_BUS_1, DEV_A, reg, seed, 2U);

    uint8_t rd[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_I2C_memRead(HW_I2C_BUS_1, DEV_A, reg, HW_I2C_MEMADDR_SIZE_8BIT, rd, 2U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(seed, rd, 2U);
}

// [test->fw~hal_i2c_004~1]
static void test_mem_write_observable_via_reg_bytes(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    const uint16_t reg = 0x30U;
    uint8_t wr[2] = { 0x77U, 0x88U };
    TEST_ASSERT_TRUE(HW_I2C_memWrite(HW_I2C_BUS_1, DEV_A, reg, HW_I2C_MEMADDR_SIZE_8BIT, wr, 2U));

    uint8_t back[2] = { 0U, 0U };
    TEST_ASSERT_EQUAL_UINT(2U, HW_I2C_sim_getRegBytes(HW_I2C_BUS_1, DEV_A, reg, back, 2U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(wr, back, 2U);
}

// [test->fw~hal_i2c_004~1]
static void test_mem_transfer_error_returns_false(void)
{
    TEST_ASSERT_TRUE(HW_I2C_init(&i2cConfig));

    HW_I2C_sim_setForceError(HW_I2C_BUS_1, true);
    uint8_t rd[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_I2C_memRead(HW_I2C_BUS_1, DEV_A, 0x00U, HW_I2C_MEMADDR_SIZE_8BIT, rd, 2U));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_unsupported_transfer_mode);
    RUN_TEST(test_init_disabled_bus_ignores_mode);

    RUN_TEST(test_two_devices_transmit_independently);
    RUN_TEST(test_two_devices_receive_independently);

    RUN_TEST(test_transmit_moves_exact_length);
    RUN_TEST(test_receive_returns_injected_bytes);
    RUN_TEST(test_timeout_formula);
    RUN_TEST(test_transfer_timeout_returns_false);
    RUN_TEST(test_transfer_nack_returns_false);

    RUN_TEST(test_mem_write_read_8bit_offset);
    RUN_TEST(test_mem_write_read_16bit_offset);
    RUN_TEST(test_mem_write_read_cypd3177_hpi_range);
    RUN_TEST(test_mem_read_returns_seeded_register_bytes);
    RUN_TEST(test_mem_write_observable_via_reg_bytes);
    RUN_TEST(test_mem_transfer_error_returns_false);

    return UNITY_END();
}
