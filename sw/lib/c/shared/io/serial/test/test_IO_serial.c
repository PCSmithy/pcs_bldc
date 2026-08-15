#include "IO_serial.h"
#include "HW_USB.h"
#include "mock_HW_USB.h"
#include "unity.h"

// File-scope config the tests build and tweak; IO_serial_init stores a pointer
// to it, so it must outlive each test.
static IO_serial_channelConfig_S channelCfg[IO_SERIAL_CHANNEL_COUNT];
static IO_serial_config_S        config;

static void buildGoodConfig(void)
{
    channelCfg[IO_SERIAL_CHANNEL_CDC] =
        (IO_serial_channelConfig_S){ .transport = IO_SERIAL_TRANSPORT_USB_CDC };
    config = (IO_serial_config_S){ .channels = channelCfg, .numChannels = IO_SERIAL_CHANNEL_COUNT };
}

void setUp(void)
{
    mock_HW_USB_reset();
    (void)HW_USB_init();
    mock_HW_USB_setConnected(true);
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- uninitialized-state check (precedes any init; static config, no reset) ---- */

// [test->fw~conn_serial_003~1]
static void test_write_before_init_is_noop(void)
{
    const uint8_t msg[1] = { 42U };
    IO_serial_write(IO_SERIAL_CHANNEL_CDC, msg, 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_USB_txLen());
}

/* ---- fw~conn_serial_001: init + config validation ---- */

// [test->fw~conn_serial_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_serial_init(&config));
}

// [test->fw~conn_serial_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_serial_init(NULL));
}

// [test->fw~conn_serial_001~1]
static void test_init_null_channels(void)
{
    config.channels = NULL;
    TEST_ASSERT_FALSE(IO_serial_init(&config));
}

// [test->fw~conn_serial_001~1]
static void test_init_too_many_channels(void)
{
    config.numChannels = IO_SERIAL_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_serial_init(&config));
}

// [test->fw~conn_serial_001~1]
static void test_init_rejects_unknown_transport(void)
{
    channelCfg[IO_SERIAL_CHANNEL_CDC].transport = IO_SERIAL_TRANSPORT_COUNT;
    TEST_ASSERT_FALSE(IO_serial_init(&config));
}

/* ---- fw~conn_serial_002: channel-addressed API ---- */

// [test->fw~conn_serial_002~1]
static void test_channel_addressed_transmit(void)
{
    TEST_ASSERT_TRUE(IO_serial_init(&config));

    const uint8_t msg[3] = { 0xA1U, 0xB2U, 0xC3U };
    IO_serial_write(IO_SERIAL_CHANNEL_CDC, msg, 3U);

    uint8_t captured[3] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(3U, mock_HW_USB_readTx(captured, 3U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, captured, 3U);
}

/* ---- fw~conn_serial_003: transmit with bounded backpressure ---- */

// [test->fw~conn_serial_003~1]
static void test_transmit_in_order(void)
{
    TEST_ASSERT_TRUE(IO_serial_init(&config));

    const uint8_t msg[5] = { 1U, 2U, 3U, 4U, 5U };
    IO_serial_write(IO_SERIAL_CHANNEL_CDC, msg, 5U);

    uint8_t captured[5] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(5U, mock_HW_USB_readTx(captured, 5U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, captured, 5U);
}

// [test->fw~conn_serial_003~1]
static void test_full_transport_drops_without_blocking(void)
{
    TEST_ASSERT_TRUE(IO_serial_init(&config));

    // Transport never accepts: the write must give up (bounded retries) and
    // return rather than hang, transmitting nothing.
    mock_HW_USB_setTxAccepting(false);
    const uint8_t msg[1] = { 0x55U };
    IO_serial_write(IO_SERIAL_CHANNEL_CDC, msg, 1U);

    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_USB_txLen());
}

/* ---- fw~conn_serial_004: byte reception ---- */

// [test->fw~conn_serial_004~1]
static void test_receive_available_and_read(void)
{
    TEST_ASSERT_TRUE(IO_serial_init(&config));
    TEST_ASSERT_EQUAL_UINT32(0U, IO_serial_available(IO_SERIAL_CHANNEL_CDC));

    const uint8_t incoming[3] = { 0x10U, 0x20U, 0x30U };
    mock_HW_USB_injectRx(incoming, 3U);
    TEST_ASSERT_EQUAL_UINT32(3U, IO_serial_available(IO_SERIAL_CHANNEL_CDC));

    uint8_t out[3] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(3U, IO_serial_read(IO_SERIAL_CHANNEL_CDC, out, 3U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(incoming, out, 3U);
    TEST_ASSERT_EQUAL_UINT32(0U, IO_serial_available(IO_SERIAL_CHANNEL_CDC));
}

/* ---- fw~conn_serial_005: connection status ---- */

// [test->fw~conn_serial_005~1]
static void test_connection_status(void)
{
    TEST_ASSERT_TRUE(IO_serial_init(&config));

    mock_HW_USB_setConnected(true);
    TEST_ASSERT_TRUE(IO_serial_connected(IO_SERIAL_CHANNEL_CDC));

    mock_HW_USB_setConnected(false);
    TEST_ASSERT_FALSE(IO_serial_connected(IO_SERIAL_CHANNEL_CDC));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_write_before_init_is_noop);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_unknown_transport);

    RUN_TEST(test_channel_addressed_transmit);

    RUN_TEST(test_transmit_in_order);
    RUN_TEST(test_full_transport_drops_without_blocking);

    RUN_TEST(test_receive_available_and_read);

    RUN_TEST(test_connection_status);

    return UNITY_END();
}
