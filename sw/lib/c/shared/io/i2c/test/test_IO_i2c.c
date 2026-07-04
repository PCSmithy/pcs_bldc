#include "IO_i2c.h"
#include "mock_HW_I2C.h"
#include "unity.h"

// File-scope config the tests build and tweak; IO_i2c_init stores a pointer to
// it, so it must outlive each test.
static IO_i2c_deviceConfig_S deviceCfg[IO_I2C_DEVICE_COUNT];
static IO_i2c_config_S       config;

// Two devices sharing one bus (BUS_B, not bus 0) at distinct addresses, with
// distinct register-offset widths so the offset-width plumbing is observable.
static void buildGoodConfig(void)
{
    deviceCfg[IO_I2C_DEVICE_0] = (IO_i2c_deviceConfig_S){
        .bus = HW_I2C_BUS_B, .devAddr7 = 0x10U, .memAddrSize = HW_I2C_MEMADDR_SIZE_16BIT };
    deviceCfg[IO_I2C_DEVICE_1] = (IO_i2c_deviceConfig_S){
        .bus = HW_I2C_BUS_B, .devAddr7 = 0x22U, .memAddrSize = HW_I2C_MEMADDR_SIZE_8BIT };

    config = (IO_i2c_config_S){ .devices = deviceCfg, .numDevices = IO_I2C_DEVICE_COUNT };
}

void setUp(void)
{
    mock_HW_I2C_reset();
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- uninitialized-state checks (must precede any successful init, since the
        driver keeps static config and has no reset hook) ---- */

// [test->fw~io_i2c_003~1]
static void test_readReg_before_init_fails(void)
{
    uint8_t buf[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(IO_i2c_readReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(mock_HW_I2C_lastCall()->called);
}

// [test->fw~io_i2c_003~1]
static void test_writeReg_before_init_fails(void)
{
    uint8_t buf[2] = { 1U, 2U };
    TEST_ASSERT_FALSE(IO_i2c_writeReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(mock_HW_I2C_lastCall()->called);
}

/* ---- fw~io_i2c_001: init + config validation ---- */

// [test->fw~io_i2c_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));
}

// [test->fw~io_i2c_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_i2c_init(NULL));
}

// [test->fw~io_i2c_001~1]
static void test_init_null_devices(void)
{
    config.devices = NULL;
    TEST_ASSERT_FALSE(IO_i2c_init(&config));
}

// [test->fw~io_i2c_001~1]
static void test_init_too_many_devices(void)
{
    config.numDevices = IO_I2C_DEVICE_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_i2c_init(&config));
}

// [test->fw~io_i2c_001~1]
static void test_init_rejects_out_of_range_bus(void)
{
    deviceCfg[IO_I2C_DEVICE_1].bus = HW_I2C_BUS_COUNT;
    TEST_ASSERT_FALSE(IO_i2c_init(&config));
}

/* ---- fw~io_i2c_002: logical device addressing ---- */

// [test->fw~io_i2c_002~1]
static void test_readReg_uses_device_bus_and_address(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    uint8_t buf[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_0, 0x1234U, buf, sizeof(buf)));

    const mock_HW_I2C_call_S * call = mock_HW_I2C_lastCall();
    TEST_ASSERT_TRUE(call->called);
    TEST_ASSERT_FALSE(call->isWrite);
    TEST_ASSERT_EQUAL(HW_I2C_BUS_B, call->bus);
    TEST_ASSERT_EQUAL_UINT8(0x10U, call->devAddr7);
    TEST_ASSERT_EQUAL_UINT16(0x1234U, call->memAddr);

    // The second device shares the bus but is reached at its own address.
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_1, 0x0056U, buf, sizeof(buf)));
    call = mock_HW_I2C_lastCall();
    TEST_ASSERT_EQUAL(HW_I2C_BUS_B, call->bus);
    TEST_ASSERT_EQUAL_UINT8(0x22U, call->devAddr7);
}

// [test->fw~io_i2c_002~1]
static void test_writeReg_uses_device_bus_and_address(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    uint8_t buf[1] = { 0xA5U };
    TEST_ASSERT_TRUE(IO_i2c_writeReg(IO_I2C_DEVICE_1, 0x0077U, buf, sizeof(buf)));

    const mock_HW_I2C_call_S * call = mock_HW_I2C_lastCall();
    TEST_ASSERT_TRUE(call->called);
    TEST_ASSERT_TRUE(call->isWrite);
    TEST_ASSERT_EQUAL(HW_I2C_BUS_B, call->bus);
    TEST_ASSERT_EQUAL_UINT8(0x22U, call->devAddr7);
    TEST_ASSERT_EQUAL_UINT16(0x0077U, call->memAddr);
}

// [test->fw~io_i2c_002~1]
static void test_out_of_range_device_fails(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    uint8_t buf[1] = { 0U };
    TEST_ASSERT_FALSE(IO_i2c_readReg(IO_I2C_DEVICE_COUNT, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(IO_i2c_writeReg(IO_I2C_DEVICE_COUNT, 0x00U, buf, sizeof(buf)));
}

/* ---- fw~io_i2c_003: register read/write with configured offset width ---- */

// [test->fw~io_i2c_003~1]
static void test_read_write_use_configured_offset_width(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    uint8_t buf[2] = { 0U, 0U };

    // DEVICE_0 is configured 16-bit.
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(HW_I2C_MEMADDR_SIZE_16BIT, mock_HW_I2C_lastCall()->memAddrSize);
    TEST_ASSERT_TRUE(IO_i2c_writeReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(HW_I2C_MEMADDR_SIZE_16BIT, mock_HW_I2C_lastCall()->memAddrSize);

    // DEVICE_1 is configured 8-bit.
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_1, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(HW_I2C_MEMADDR_SIZE_8BIT, mock_HW_I2C_lastCall()->memAddrSize);
    TEST_ASSERT_TRUE(IO_i2c_writeReg(IO_I2C_DEVICE_1, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(HW_I2C_MEMADDR_SIZE_8BIT, mock_HW_I2C_lastCall()->memAddrSize);
}

// [test->fw~io_i2c_003~1]
static void test_readReg_returns_injected_bytes(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    const uint8_t injected[3] = { 0xDEU, 0xADU, 0xBEU };
    mock_HW_I2C_setResponse(injected, sizeof(injected));

    uint8_t buf[3] = { 0U, 0U, 0U };
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_0, 0x0010U, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(injected, buf, sizeof(injected));
    TEST_ASSERT_EQUAL_size_t(sizeof(buf), mock_HW_I2C_lastCall()->length);
}

// [test->fw~io_i2c_003~1]
static void test_writeReg_forwards_caller_bytes(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    uint8_t payload[3] = { 0xBEU, 0xEFU, 0x42U };
    TEST_ASSERT_TRUE(IO_i2c_writeReg(IO_I2C_DEVICE_0, 0x0010U, payload, sizeof(payload)));

    const mock_HW_I2C_call_S * call = mock_HW_I2C_lastCall();
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), call->length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, call->data, sizeof(payload));
}

// [test->fw~io_i2c_003~1]
static void test_read_write_forward_hw_failure(void)
{
    TEST_ASSERT_TRUE(IO_i2c_init(&config));

    uint8_t buf[1] = { 0U };

    mock_HW_I2C_setTransferOk(false);
    TEST_ASSERT_FALSE(IO_i2c_readReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(IO_i2c_writeReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));

    mock_HW_I2C_setTransferOk(true);
    TEST_ASSERT_TRUE(IO_i2c_readReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(IO_i2c_writeReg(IO_I2C_DEVICE_0, 0x00U, buf, sizeof(buf)));
}

int main(void)
{
    UNITY_BEGIN();

    // Uninitialized-state checks first.
    RUN_TEST(test_readReg_before_init_fails);
    RUN_TEST(test_writeReg_before_init_fails);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_devices);
    RUN_TEST(test_init_too_many_devices);
    RUN_TEST(test_init_rejects_out_of_range_bus);

    RUN_TEST(test_readReg_uses_device_bus_and_address);
    RUN_TEST(test_writeReg_uses_device_bus_and_address);
    RUN_TEST(test_out_of_range_device_fails);

    RUN_TEST(test_read_write_use_configured_offset_width);
    RUN_TEST(test_readReg_returns_injected_bytes);
    RUN_TEST(test_writeReg_forwards_caller_bytes);
    RUN_TEST(test_read_write_forward_hw_failure);

    return UNITY_END();
}
