#include "dev_gateDriver.h"
#include "mock_IO_i2c.h"
#include "unity.h"

// Driven through dev_gateDriver_run200ms against a mock IO_i2c that stores
// written bytes (so the driver's readback-verify runs against its own writes).

// Board-plausible configuration values: VCC=10V, datasheet defaults elsewhere.
#define CFG_POWMNG (0x01U)
#define CFG_LOGIC  (0x73U)
#define CFG_READY  (0x09U)
#define CFG_NFAULT (0x7FU)

// dev_gateDriver_init stores a pointer to the config, so it must outlive each test.
static dev_gateDriver_channelConfig_S channelCfg[DEV_GATEDRIVER_CHANNEL_COUNT];
static dev_gateDriver_config_S        config;

static void buildGoodConfig(void)
{
    channelCfg[DEV_GATEDRIVER_CHANNEL_A] = (dev_gateDriver_channelConfig_S){
        .ioDevice = IO_I2C_DEVICE_0,
        .powmng = CFG_POWMNG, .logic = CFG_LOGIC, .ready = CFG_READY, .nfault = CFG_NFAULT,
    };
    channelCfg[DEV_GATEDRIVER_CHANNEL_B] = (dev_gateDriver_channelConfig_S){
        .ioDevice = IO_I2C_DEVICE_1,
        .powmng = CFG_POWMNG, .logic = CFG_LOGIC, .ready = CFG_READY, .nfault = CFG_NFAULT,
    };
    config = (dev_gateDriver_config_S){ .channels = channelCfg, .numChannels = DEV_GATEDRIVER_CHANNEL_COUNT };
}

void setUp(void)
{
    mock_IO_i2c_reset();
    buildGoodConfig();
}

void tearDown(void) {}

// The driver keeps static config with no de-init hook, so the uninitialized-state
// checks must run before any successful init.

// [test->fw~mc_004~1]
static void test_accessors_before_init_return_defaults(void)
{
    TEST_ASSERT_FALSE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_A));

    dev_gateDriver_snapshot_S snap;
    snap.statusRaw = 0xA5U;   // must be cleared by the failed call
    TEST_ASSERT_FALSE(dev_gateDriver_getSnapshot(DEV_GATEDRIVER_CHANNEL_A, &snap));
    TEST_ASSERT_FALSE(snap.configured);
    TEST_ASSERT_EQUAL_UINT8(0U, snap.statusRaw);
}

// [test->fw~mc_005~1]
static void test_clear_faults_before_init_rejected(void)
{
    TEST_ASSERT_FALSE(dev_gateDriver_clearFaults(DEV_GATEDRIVER_CHANNEL_A));
    TEST_ASSERT_EQUAL_size_t(0U, mock_IO_i2c_writeCount());
}

// [test->fw~mc_001~1]
static void test_run200ms_before_init_is_noop(void)
{
    dev_gateDriver_run200ms();
    TEST_ASSERT_EQUAL_size_t(0U, mock_IO_i2c_readCount());
    TEST_ASSERT_EQUAL_size_t(0U, mock_IO_i2c_writeCount());
}

// [test->fw~mc_001~1]
static void test_init_null_config_false(void)
{
    TEST_ASSERT_FALSE(dev_gateDriver_init(NULL));
}

// [test->fw~mc_001~1]
static void test_init_null_channels_false(void)
{
    config.channels = NULL;
    TEST_ASSERT_FALSE(dev_gateDriver_init(&config));
}

// [test->fw~mc_001~1]
static void test_init_wrong_channel_count_false(void)
{
    config.numChannels = DEV_GATEDRIVER_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(dev_gateDriver_init(&config));
}

// [test->fw~mc_001~1]
static void test_init_rejects_out_of_range_device(void)
{
    channelCfg[DEV_GATEDRIVER_CHANNEL_A].ioDevice = IO_I2C_DEVICE_COUNT;
    TEST_ASSERT_FALSE(dev_gateDriver_init(&config));
}

// [test->fw~mc_001~1]
static void test_init_valid_config_true(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    TEST_ASSERT_FALSE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_A));
}

// [test->fw~mc_002~1]
static void test_configure_sequence_success_and_order(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));

    dev_gateDriver_run200ms();

    TEST_ASSERT_TRUE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_A));
    TEST_ASSERT_TRUE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_B));

    // Channel A's write order: unlock, POWMNG, LOGIC, READY, NFAULT, relock, clear.
    mock_IO_i2c_write_S writes[MOCK_IO_I2C_MAX_WRITES];
    const size_t n = mock_IO_i2c_getWrites(writes, MOCK_IO_I2C_MAX_WRITES);
    TEST_ASSERT_EQUAL_size_t(7U * DEV_GATEDRIVER_CHANNEL_COUNT, n);

    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_LOCK, writes[0].reg);
    TEST_ASSERT_EQUAL_UINT8(DEV_GATEDRIVER_LOCK_UNLOCK, writes[0].value);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_POWMNG, writes[1].reg);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_LOGIC, writes[2].reg);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_READY, writes[3].reg);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_NFAULT, writes[4].reg);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_LOCK, writes[5].reg);
    TEST_ASSERT_EQUAL_UINT8(DEV_GATEDRIVER_LOCK_RELOCK, writes[5].value);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)DEV_GATEDRIVER_REG_CLEAR, writes[6].reg);
    TEST_ASSERT_EQUAL_UINT8(DEV_GATEDRIVER_CLEAR_ALL, writes[6].value);

    // The device registers hold the configured values.
    uint8_t stored = 0U;
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_POWMNG, &stored, 1U));
    TEST_ASSERT_EQUAL_UINT8(CFG_POWMNG, stored);
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_LOGIC, &stored, 1U));
    TEST_ASSERT_EQUAL_UINT8(CFG_LOGIC, stored);
}

// [test->fw~mc_002~1]
static void test_configure_write_failure_leaves_unconfigured_and_retries(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    mock_IO_i2c_failWriteReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_POWMNG);

    dev_gateDriver_run200ms();
    TEST_ASSERT_FALSE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_A));
    TEST_ASSERT_TRUE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_B));   // independent
    const size_t writesAfterFirst = mock_IO_i2c_writeCount();

    // A subsequent fetch repeats the sequence for the failed channel only.
    dev_gateDriver_run200ms();
    TEST_ASSERT_FALSE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_A));
    TEST_ASSERT_EQUAL_size_t(writesAfterFirst + 7U, mock_IO_i2c_writeCount());
}

// [test->fw~mc_002~1]
static void test_configure_readback_mismatch_leaves_unconfigured(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    // LOGIC reports success on write but reads back a different value.
    static const uint8_t wrongLogic = 0x00U;
    mock_IO_i2c_stickReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_LOGIC, &wrongLogic, 1U);

    dev_gateDriver_run200ms();
    TEST_ASSERT_FALSE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_A));
}

// [test->fw~mc_003~1]
static void test_status_fetch_decodes_flags(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    static const uint8_t status0x0D = 0x0DU;   // RESET | VDS_P | VCC_UVLO
    mock_IO_i2c_setReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_STATUS, &status0x0D, 1U);

    dev_gateDriver_run200ms();   // configures, then reads STATUS

    dev_gateDriver_snapshot_S snap;
    TEST_ASSERT_TRUE(dev_gateDriver_getSnapshot(DEV_GATEDRIVER_CHANNEL_A, &snap));
    TEST_ASSERT_TRUE(snap.configured);
    TEST_ASSERT_TRUE(snap.statusOk);
    TEST_ASSERT_EQUAL_UINT8(0x0DU, snap.statusRaw);
    TEST_ASSERT_TRUE(snap.resetLatched);
    TEST_ASSERT_TRUE(snap.vdsProtection);
    TEST_ASSERT_TRUE(snap.vccUndervoltage);
    TEST_ASSERT_FALSE(snap.thermalShutdown);
    TEST_ASSERT_FALSE(snap.locked);
}

// [test->fw~mc_003~1]
static void test_status_read_failure_retains_flags(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    static const uint8_t status0x0D = 0x0DU;
    mock_IO_i2c_setReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_STATUS, &status0x0D, 1U);
    dev_gateDriver_run200ms();

    mock_IO_i2c_failReg(IO_I2C_DEVICE_0, (uint16_t)DEV_GATEDRIVER_REG_STATUS);
    dev_gateDriver_run200ms();

    dev_gateDriver_snapshot_S snap;
    TEST_ASSERT_TRUE(dev_gateDriver_getSnapshot(DEV_GATEDRIVER_CHANNEL_A, &snap));
    TEST_ASSERT_FALSE(snap.statusOk);                 // failure recorded
    TEST_ASSERT_EQUAL_UINT8(0x0DU, snap.statusRaw);   // last-good retained
    TEST_ASSERT_TRUE(snap.vdsProtection);
}

// [test->fw~mc_004~1]
static void test_out_of_range_channel_accessors_default(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    TEST_ASSERT_FALSE(dev_gateDriver_isConfigured(DEV_GATEDRIVER_CHANNEL_COUNT));

    dev_gateDriver_snapshot_S snap;
    snap.statusRaw = 0xA5U;
    TEST_ASSERT_FALSE(dev_gateDriver_getSnapshot(DEV_GATEDRIVER_CHANNEL_COUNT, &snap));
    TEST_ASSERT_EQUAL_UINT8(0U, snap.statusRaw);
    TEST_ASSERT_FALSE(dev_gateDriver_getSnapshot(DEV_GATEDRIVER_CHANNEL_A, NULL));
}

// [test->fw~mc_005~1]
static void test_clear_faults_writes_clear_register(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));

    const size_t writesBefore = mock_IO_i2c_writeCount();
    TEST_ASSERT_TRUE(dev_gateDriver_clearFaults(DEV_GATEDRIVER_CHANNEL_A));
    TEST_ASSERT_EQUAL_size_t(writesBefore + 1U, mock_IO_i2c_writeCount());

    mock_IO_i2c_write_S writes[MOCK_IO_I2C_MAX_WRITES];
    const size_t n = mock_IO_i2c_getWrites(writes, MOCK_IO_I2C_MAX_WRITES);
    TEST_ASSERT_EQUAL_UINT16(0x09U, writes[n - 1U].reg);
    TEST_ASSERT_EQUAL_UINT8(0xFFU, writes[n - 1U].value);
}

// [test->fw~mc_005~1]
static void test_clear_faults_out_of_range_rejected(void)
{
    TEST_ASSERT_TRUE(dev_gateDriver_init(&config));
    const size_t writesBefore = mock_IO_i2c_writeCount();
    TEST_ASSERT_FALSE(dev_gateDriver_clearFaults(DEV_GATEDRIVER_CHANNEL_COUNT));
    TEST_ASSERT_EQUAL_size_t(writesBefore, mock_IO_i2c_writeCount());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_accessors_before_init_return_defaults);
    RUN_TEST(test_clear_faults_before_init_rejected);
    RUN_TEST(test_run200ms_before_init_is_noop);

    RUN_TEST(test_init_null_config_false);
    RUN_TEST(test_init_null_channels_false);
    RUN_TEST(test_init_wrong_channel_count_false);
    RUN_TEST(test_init_rejects_out_of_range_device);
    RUN_TEST(test_init_valid_config_true);

    RUN_TEST(test_configure_sequence_success_and_order);
    RUN_TEST(test_configure_write_failure_leaves_unconfigured_and_retries);
    RUN_TEST(test_configure_readback_mismatch_leaves_unconfigured);

    RUN_TEST(test_status_fetch_decodes_flags);
    RUN_TEST(test_status_read_failure_retains_flags);

    RUN_TEST(test_out_of_range_channel_accessors_default);
    RUN_TEST(test_clear_faults_writes_clear_register);
    RUN_TEST(test_clear_faults_out_of_range_rejected);

    return UNITY_END();
}
