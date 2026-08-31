#include "IO_COBSFrame.h"
#include "IO_serial.h"
#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "unity.h"

/* Test fixtures */

static IO_serial_channelConfig_S serialChannelCfg[IO_SERIAL_CHANNEL_COUNT];
static IO_serial_config_S        serialConfig;
static IO_COBSFrame_channelConfig_S  frameChannelCfg[IO_COBSFRAME_CHANNEL_COUNT];
static IO_COBSFrame_config_S         frameConfig;

static void buildGoodConfigs(void)
{
    serialChannelCfg[IO_SERIAL_CHANNEL_CDC] =
        (IO_serial_channelConfig_S){ .transport = IO_SERIAL_TRANSPORT_USB_CDC };
    serialConfig = (IO_serial_config_S){
        .channels = serialChannelCfg, .numChannels = IO_SERIAL_CHANNEL_COUNT };

    frameChannelCfg[IO_COBSFRAME_CHANNEL_CDC] = (IO_COBSFrame_channelConfig_S){
        .serialChannel = IO_SERIAL_CHANNEL_CDC,
        .maxFrameLen   = IO_COBSFRAME_MAX_PAYLOAD,
    };
    frameConfig = (IO_COBSFrame_config_S){
        .channels = frameChannelCfg, .numChannels = IO_COBSFRAME_CHANNEL_COUNT };
}

void setUp(void)
{
    HW_USB_sim_reset();
    (void)HW_USB_init();
    HW_USB_sim_setConnected(true);
    buildGoodConfigs();
    TEST_ASSERT_TRUE(IO_serial_init(&serialConfig));
    TEST_ASSERT_TRUE(IO_COBSFrame_init(&frameConfig));
}

void tearDown(void) {}

// Capture everything the frame layer transmitted, then re-present it as
// received bytes (the sim's TX and RX are independent, so loopback is manual).
static uint32_t loopTxToRx(void)
{
    uint8_t wire[512];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    HW_USB_sim_reset();
    HW_USB_sim_setConnected(true);
    HW_USB_sim_injectRx(wire, wireLen);
    return wireLen;
}

/* ---- fw~conn_proto_003: init + config validation ---- */

// [test->fw~conn_proto_003~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_COBSFrame_init(&frameConfig));
}

// [test->fw~conn_proto_003~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_COBSFrame_init(NULL));
}

// [test->fw~conn_proto_003~1]
static void test_init_null_channels(void)
{
    frameConfig.channels = NULL;
    TEST_ASSERT_FALSE(IO_COBSFrame_init(&frameConfig));
}

// [test->fw~conn_proto_003~1]
static void test_init_too_many_channels(void)
{
    frameConfig.numChannels = IO_COBSFRAME_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_COBSFrame_init(&frameConfig));
}

// [test->fw~conn_proto_003~1]
static void test_init_rejects_bad_serial_channel(void)
{
    frameChannelCfg[IO_COBSFRAME_CHANNEL_CDC].serialChannel = IO_SERIAL_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(IO_COBSFrame_init(&frameConfig));
}

// [test->fw~conn_proto_003~1]
static void test_init_rejects_zero_max_frame_len(void)
{
    frameChannelCfg[IO_COBSFRAME_CHANNEL_CDC].maxFrameLen = 0U;
    TEST_ASSERT_FALSE(IO_COBSFrame_init(&frameConfig));
}

// [test->fw~conn_proto_003~1]
static void test_init_rejects_oversized_max_frame_len(void)
{
    frameChannelCfg[IO_COBSFRAME_CHANNEL_CDC].maxFrameLen = IO_COBSFRAME_MAX_PAYLOAD + 1U;
    TEST_ASSERT_FALSE(IO_COBSFrame_init(&frameConfig));
}

/* ---- fw~conn_proto_002: frame format ---- */

// [test->fw~conn_proto_002~1]
static void test_wire_format_reference_vector(void)
{
    // Payload "123456789" has the standard CRC-32 check value 0xCBF43926;
    // payload+CRC contain no zeros, so COBS is a single 14-byte block.
    const uint8_t payload[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    const uint8_t expected[16] = {
        0x00U, 0x0EU,
        '1', '2', '3', '4', '5', '6', '7', '8', '9',
        0x26U, 0x39U, 0xF4U, 0xCBU,
        0x00U,
    };
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, payload, 9U));

    uint8_t wire[32] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(16U, HW_USB_sim_readTx(wire, sizeof(wire)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, wire, 16U);
}

// [test->fw~conn_proto_002~1]
static void test_wire_body_contains_no_zero_bytes(void)
{
    uint8_t payload[64];
    for (size_t i = 0U; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)((i % 5U == 0U) ? 0U : (i & 0xFFU));
    }
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, payload, sizeof(payload)));

    uint8_t wire[128] = { 0U };
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    TEST_ASSERT_TRUE(wireLen > 2U);
    TEST_ASSERT_EQUAL_UINT8(0x00U, wire[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, wire[wireLen - 1U]);
    for (uint32_t i = 1U; i < (wireLen - 1U); i++)
    {
        TEST_ASSERT_NOT_EQUAL(0x00U, wire[i]);
    }
}

/* ---- fw~conn_proto_004: whole-frame transmission ---- */

// [test->fw~conn_proto_004~1]
static void test_frames_transmit_in_order(void)
{
    const uint8_t a[3] = { 0xA1U, 0xA2U, 0xA3U };
    const uint8_t b[3] = { 0xB1U, 0xB2U, 0xB3U };
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, a, 3U));
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, b, 3U));

    (void)loopTxToRx();
    IO_COBSFrame_run();

    uint8_t frame[IO_COBSFRAME_MAX_PAYLOAD];
    size_t frameLen = 0U;
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(3U, frameLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, frame, 3U);

    IO_COBSFrame_run();
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(3U, frameLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, frame, 3U);
}

// [test->fw~conn_proto_004~1]
static void test_frame_exceeding_capacity_dropped_whole(void)
{
    // Leave less free transmit space than one encoded frame needs.
    uint8_t filler[64];
    for (size_t i = 0U; i < sizeof(filler); i++)
    {
        filler[i] = 0x55U;
    }
    while (IO_serial_txFree(IO_SERIAL_CHANNEL_CDC) > 8U)
    {
        const uint32_t chunk =
            (IO_serial_txFree(IO_SERIAL_CHANNEL_CDC) - 8U > sizeof(filler))
                ? (uint32_t)sizeof(filler)
                : (IO_serial_txFree(IO_SERIAL_CHANNEL_CDC) - 8U);
        IO_serial_write(IO_SERIAL_CHANNEL_CDC, filler, chunk);
    }
    const uint32_t txLenBefore = HW_USB_sim_txLen();

    const uint8_t payload[16] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
                                  9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U };
    TEST_ASSERT_FALSE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT32(txLenBefore, HW_USB_sim_txLen());
}

/* ---- fw~conn_proto_005: reception + resynchronization ---- */

// [test->fw~conn_proto_005~1]
static void test_roundtrip_including_zero_length(void)
{
    const uint8_t payload[5] = { 0x10U, 0x00U, 0x30U, 0x00U, 0x50U };
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, payload, 5U));
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, NULL, 0U));

    (void)loopTxToRx();
    IO_COBSFrame_run();

    uint8_t frame[IO_COBSFRAME_MAX_PAYLOAD];
    size_t frameLen = 0U;
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(5U, frameLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, frame, 5U);

    IO_COBSFrame_run();
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(0U, frameLen);
}

// [test->fw~conn_proto_005~1]
static void test_corrupted_frame_discarded_next_delivered(void)
{
    const uint8_t a[4] = { 0xAAU, 0xABU, 0xACU, 0xADU };
    const uint8_t b[4] = { 0xBAU, 0xBBU, 0xBCU, 0xBDU };
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, a, 4U));
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, b, 4U));

    uint8_t wire[64];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    // Corrupt a body byte of frame A without creating a delimiter.
    wire[3] = (uint8_t)(wire[3] ^ 0x40U);
    HW_USB_sim_reset();
    HW_USB_sim_setConnected(true);
    HW_USB_sim_injectRx(wire, wireLen);

    IO_COBSFrame_run();
    uint8_t frame[IO_COBSFRAME_MAX_PAYLOAD];
    size_t frameLen = 0U;
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(4U, frameLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, frame, 4U);
}

// [test->fw~conn_proto_005~1]
static void test_overlong_garbage_discarded_next_delivered(void)
{
    // 300 delimiter-free bytes exceed the assembly bound for a 256-byte
    // payload; the segment must be discarded, then the valid frame delivered.
    uint8_t garbage[300];
    for (size_t i = 0U; i < sizeof(garbage); i++)
    {
        garbage[i] = 0x77U;
    }
    HW_USB_sim_injectRx(garbage, sizeof(garbage));
    const uint8_t delimiter = 0x00U;
    HW_USB_sim_injectRx(&delimiter, 1U);

    const uint8_t payload[3] = { 0x01U, 0x02U, 0x03U };
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, payload, 3U));
    uint8_t wire[32];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    HW_USB_sim_injectRx(wire, wireLen);

    IO_COBSFrame_run();
    uint8_t frame[IO_COBSFRAME_MAX_PAYLOAD];
    size_t frameLen = 0U;
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(3U, frameLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, frame, 3U);
}

// [test->fw~conn_proto_005~1]
static void test_empty_segments_skipped(void)
{
    const uint8_t delimiters[3] = { 0x00U, 0x00U, 0x00U };
    HW_USB_sim_injectRx(delimiters, 3U);

    const uint8_t payload[2] = { 0xC1U, 0xC2U };
    TEST_ASSERT_TRUE(IO_COBSFrame_send(IO_COBSFRAME_CHANNEL_CDC, payload, 2U));
    uint8_t wire[32];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    HW_USB_sim_injectRx(wire, wireLen);

    IO_COBSFrame_run();
    uint8_t frame[IO_COBSFRAME_MAX_PAYLOAD];
    size_t frameLen = 0U;
    TEST_ASSERT_TRUE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
    TEST_ASSERT_EQUAL_size_t(2U, frameLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, frame, 2U);
}

static void test_receive_without_frame_returns_false(void)
{
    uint8_t frame[IO_COBSFRAME_MAX_PAYLOAD];
    size_t frameLen = 0U;
    TEST_ASSERT_FALSE(IO_COBSFrame_receive(IO_COBSFRAME_CHANNEL_CDC, frame, sizeof(frame), &frameLen));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_bad_serial_channel);
    RUN_TEST(test_init_rejects_zero_max_frame_len);
    RUN_TEST(test_init_rejects_oversized_max_frame_len);

    RUN_TEST(test_wire_format_reference_vector);
    RUN_TEST(test_wire_body_contains_no_zero_bytes);

    RUN_TEST(test_frames_transmit_in_order);
    RUN_TEST(test_frame_exceeding_capacity_dropped_whole);

    RUN_TEST(test_roundtrip_including_zero_length);
    RUN_TEST(test_corrupted_frame_discarded_next_delivered);
    RUN_TEST(test_overlong_garbage_discarded_next_delivered);
    RUN_TEST(test_empty_segments_skipped);
    RUN_TEST(test_receive_without_frame_returns_false);

    return UNITY_END();
}
