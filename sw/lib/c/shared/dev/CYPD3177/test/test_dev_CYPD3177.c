#include "dev_CYPD3177.h"
#include "mock_IO_i2c.h"
#include "CYPD3177.h"
#include "unity.h"

// Driven through dev_CYPD3177_run200ms against a mock IO_i2c; decode math is
// lib_CYPD3177's (fw~pd_001..004) and is not re-checked here.

// A known 20 V / 3 A contract as the CYPD3177's little-endian register bytes.
static const uint8_t DEVICE_MODE_ALIVE  = 0x95U;
static const uint8_t PD_STATUS_CONTRACT[4] = { 0x00U, 0x04U, 0x00U, 0x00U };  // bit10 set
static const uint8_t BUS_VOLTAGE_20V    = 0xC8U;             // 200 * 100 mV = 20000 mV
static const uint8_t CURRENT_PDO_20V[4] = { 0x00U, 0x40U, 0x06U, 0x00U };     // field 400 -> 20000 mV
static const uint8_t CURRENT_RDO_3A[4]  = { 0x00U, 0xB0U, 0x04U, 0x00U };     // field 300 -> 3000 mA

// dev_CYPD3177_init stores a pointer to the config, so it must outlive each test.
static dev_CYPD3177_channelConfig_S channelCfg[DEV_CYPD3177_CHANNEL_COUNT];
static dev_CYPD3177_config_S        config;

static void buildGoodConfig(void)
{
    channelCfg[DEV_CYPD3177_CHANNEL_A] = (dev_CYPD3177_channelConfig_S){ .ioDevice = IO_I2C_DEVICE_0 };
    channelCfg[DEV_CYPD3177_CHANNEL_B] = (dev_CYPD3177_channelConfig_S){ .ioDevice = IO_I2C_DEVICE_1 };
    config = (dev_CYPD3177_config_S){ .channels = channelCfg, .numChannels = DEV_CYPD3177_CHANNEL_COUNT };
}

static void injectContract(IO_i2c_device_E dev, uint8_t deviceMode,
                           const uint8_t pdStatus[4], uint8_t busVoltage,
                           const uint8_t currentPdo[4], const uint8_t currentRdo[4])
{
    mock_IO_i2c_setReg(dev, (uint16_t)CYPD3177_REG_DEVICE_MODE, &deviceMode, 1U);
    mock_IO_i2c_setReg(dev, (uint16_t)CYPD3177_REG_PD_STATUS, pdStatus, 4U);
    mock_IO_i2c_setReg(dev, (uint16_t)CYPD3177_REG_BUS_VOLTAGE, &busVoltage, 1U);
    mock_IO_i2c_setReg(dev, (uint16_t)CYPD3177_REG_CURRENT_PDO, currentPdo, 4U);
    mock_IO_i2c_setReg(dev, (uint16_t)CYPD3177_REG_CURRENT_RDO, currentRdo, 4U);
}

void setUp(void)
{
    mock_IO_i2c_reset();
    buildGoodConfig();
}

void tearDown(void) {}

// The driver keeps static config with no de-init hook, so the uninitialized-state
// checks must run before any successful init.

// [test->fw~pd_005~1]
static void test_accessors_before_init_return_defaults(void)
{
    TEST_ASSERT_FALSE(dev_CYPD3177_isPresent(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_FALSE(dev_CYPD3177_isContractActive(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(0U, dev_CYPD3177_negotiatedVoltage_mV(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(0U, dev_CYPD3177_negotiatedCurrent_mA(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(0U, dev_CYPD3177_busVoltage_mV(DEV_CYPD3177_CHANNEL_A));
}

// [test->fw~pd_005~1]
static void test_run200ms_before_init_is_noop(void)
{
    injectContract(IO_I2C_DEVICE_0, DEVICE_MODE_ALIVE, PD_STATUS_CONTRACT, BUS_VOLTAGE_20V,
                   CURRENT_PDO_20V, CURRENT_RDO_3A);
    dev_CYPD3177_run200ms();
    TEST_ASSERT_EQUAL_size_t(0U, mock_IO_i2c_readCount());
}

// [test->fw~pd_005~1]
static void test_init_null_config_false(void)
{
    TEST_ASSERT_FALSE(dev_CYPD3177_init(NULL));
}

// [test->fw~pd_005~1]
static void test_init_null_channels_false(void)
{
    config.channels = NULL;
    TEST_ASSERT_FALSE(dev_CYPD3177_init(&config));
}

// [test->fw~pd_005~1]
static void test_init_wrong_channel_count_false(void)
{
    config.numChannels = DEV_CYPD3177_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(dev_CYPD3177_init(&config));
}

// [test->fw~pd_005~1]
static void test_init_rejects_out_of_range_device(void)
{
    channelCfg[DEV_CYPD3177_CHANNEL_A].ioDevice = IO_I2C_DEVICE_COUNT;
    TEST_ASSERT_FALSE(dev_CYPD3177_init(&config));
}

// [test->fw~pd_005~1]
static void test_init_valid_config_true(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));
}

// [test->fw~pd_006~1]
// [test->fw~pd_007~1]
static void test_run200ms_populates_and_exposes_status(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));
    injectContract(IO_I2C_DEVICE_0, DEVICE_MODE_ALIVE, PD_STATUS_CONTRACT, BUS_VOLTAGE_20V,
                   CURRENT_PDO_20V, CURRENT_RDO_3A);

    dev_CYPD3177_run200ms();

    TEST_ASSERT_TRUE(dev_CYPD3177_isPresent(DEV_CYPD3177_CHANNEL_A));
    // bit10 sits in byte1, so reading contract-active proves little-endian assembly.
    TEST_ASSERT_TRUE(dev_CYPD3177_isContractActive(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(20000U, dev_CYPD3177_negotiatedVoltage_mV(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(3000U,  dev_CYPD3177_negotiatedCurrent_mA(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(20000U, dev_CYPD3177_busVoltage_mV(DEV_CYPD3177_CHANNEL_A));
}

// [test->fw~pd_006~1]
static void test_run200ms_reads_each_register(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));
    injectContract(IO_I2C_DEVICE_0, DEVICE_MODE_ALIVE, PD_STATUS_CONTRACT, BUS_VOLTAGE_20V,
                   CURRENT_PDO_20V, CURRENT_RDO_3A);

    dev_CYPD3177_run200ms();
    TEST_ASSERT_EQUAL_size_t(5U * DEV_CYPD3177_CHANNEL_COUNT, mock_IO_i2c_readCount());
}

// [test->fw~pd_007~1]
static void test_channels_expose_independent_status(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));

    injectContract(IO_I2C_DEVICE_0, DEVICE_MODE_ALIVE, PD_STATUS_CONTRACT, BUS_VOLTAGE_20V,
                   CURRENT_PDO_20V, CURRENT_RDO_3A);
    static const uint8_t busVoltage5V[1]  = { 0x32U };                            // 50 -> 5000 mV
    static const uint8_t currentPdo5V[4]  = { 0x00U, 0x90U, 0x01U, 0x00U };       // field 100 -> 5000 mV
    static const uint8_t currentRdo15A[4] = { 0x00U, 0x58U, 0x02U, 0x00U };       // field 150 -> 1500 mA
    injectContract(IO_I2C_DEVICE_1, DEVICE_MODE_ALIVE, PD_STATUS_CONTRACT, busVoltage5V[0],
                   currentPdo5V, currentRdo15A);

    // Distinct per-device data also proves each channel reads its own device.
    dev_CYPD3177_run200ms();

    TEST_ASSERT_EQUAL_UINT32(20000U, dev_CYPD3177_negotiatedVoltage_mV(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(3000U,  dev_CYPD3177_negotiatedCurrent_mA(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(5000U,  dev_CYPD3177_negotiatedVoltage_mV(DEV_CYPD3177_CHANNEL_B));
    TEST_ASSERT_EQUAL_UINT32(1500U,  dev_CYPD3177_negotiatedCurrent_mA(DEV_CYPD3177_CHANNEL_B));
    TEST_ASSERT_EQUAL_UINT32(5000U,  dev_CYPD3177_busVoltage_mV(DEV_CYPD3177_CHANNEL_B));
}

// [test->fw~pd_008~1]
static void test_device_mode_zero_reports_not_present(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));
    static const uint8_t deviceModeZero = 0x00U;
    mock_IO_i2c_setReg(IO_I2C_DEVICE_0, (uint16_t)CYPD3177_REG_DEVICE_MODE, &deviceModeZero, 1U);

    dev_CYPD3177_run200ms();   // every read succeeds; only DEVICE_MODE is zero
    TEST_ASSERT_FALSE(dev_CYPD3177_isPresent(DEV_CYPD3177_CHANNEL_A));
}

// [test->fw~pd_006~1]
// [test->fw~pd_008~1]
static void test_read_failure_reports_not_present_and_retains(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));
    injectContract(IO_I2C_DEVICE_0, DEVICE_MODE_ALIVE, PD_STATUS_CONTRACT, BUS_VOLTAGE_20V,
                   CURRENT_PDO_20V, CURRENT_RDO_3A);
    dev_CYPD3177_run200ms();
    TEST_ASSERT_TRUE(dev_CYPD3177_isPresent(DEV_CYPD3177_CHANNEL_A));

    mock_IO_i2c_failReg(IO_I2C_DEVICE_0, (uint16_t)CYPD3177_REG_CURRENT_PDO);
    dev_CYPD3177_run200ms();

    // Not present, but the last-good values survive the failed read.
    TEST_ASSERT_FALSE(dev_CYPD3177_isPresent(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_TRUE(dev_CYPD3177_isContractActive(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(20000U, dev_CYPD3177_negotiatedVoltage_mV(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(3000U,  dev_CYPD3177_negotiatedCurrent_mA(DEV_CYPD3177_CHANNEL_A));
    TEST_ASSERT_EQUAL_UINT32(20000U, dev_CYPD3177_busVoltage_mV(DEV_CYPD3177_CHANNEL_A));
}

// [test->fw~pd_007~1]
static void test_out_of_range_channel_accessors_default(void)
{
    TEST_ASSERT_TRUE(dev_CYPD3177_init(&config));
    TEST_ASSERT_FALSE(dev_CYPD3177_isPresent(DEV_CYPD3177_CHANNEL_COUNT));
    TEST_ASSERT_EQUAL_UINT32(0U, dev_CYPD3177_negotiatedVoltage_mV(DEV_CYPD3177_CHANNEL_COUNT));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_accessors_before_init_return_defaults);
    RUN_TEST(test_run200ms_before_init_is_noop);

    RUN_TEST(test_init_null_config_false);
    RUN_TEST(test_init_null_channels_false);
    RUN_TEST(test_init_wrong_channel_count_false);
    RUN_TEST(test_init_rejects_out_of_range_device);
    RUN_TEST(test_init_valid_config_true);

    RUN_TEST(test_run200ms_populates_and_exposes_status);
    RUN_TEST(test_run200ms_reads_each_register);
    RUN_TEST(test_channels_expose_independent_status);

    RUN_TEST(test_device_mode_zero_reports_not_present);
    RUN_TEST(test_read_failure_reports_not_present_and_retains);
    RUN_TEST(test_out_of_range_channel_accessors_default);

    return UNITY_END();
}
