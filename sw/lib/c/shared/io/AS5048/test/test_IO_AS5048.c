#include "IO_AS5048.h"
#include "mock_HW_SPI.h"
#include "unity.h"

// File-scope config the tests build and tweak; IO_AS5048_init stores a pointer
// to it, so it must outlive each test.
static IO_AS5048_channelConfig_S channelCfg[IO_AS5048_CHANNEL_COUNT];
static IO_AS5048_config_S        config;

// Build a 16-bit AS5048 response frame: 14-bit angle in bits[13:0], error flag
// in bit14, and bit15 chosen so the whole frame has EVEN parity. validParity
// = false flips bit15 to corrupt the parity.
static uint16_t makeFrame(uint16_t angle14, bool errorFlag, bool validParity)
{
    uint16_t frame = (uint16_t)(angle14 & 0x3FFFU);
    if (errorFlag) { frame |= 0x4000U; }

    uint16_t ones = 0U;
    for (uint16_t bit = 0U; bit < 15U; bit++)
    {
        if ((frame & (uint16_t)(1U << bit)) != 0U) { ones++; }
    }
    if ((ones & 1U) != 0U) { frame |= 0x8000U; }   // even total set-bit count
    if (!validParity)      { frame ^= 0x8000U; }   // corrupt -> odd count
    return frame;
}

// Two encoders: ENC_A forward, ENC_B reverse, on distinct SPI channels.
static void buildGoodConfig(void)
{
    channelCfg[IO_AS5048_CHANNEL_ENC_A] =
        (IO_AS5048_channelConfig_S){ .spiChannel = HW_SPI_CHANNEL_ENC_A, .reverse = false };
    channelCfg[IO_AS5048_CHANNEL_ENC_B] =
        (IO_AS5048_channelConfig_S){ .spiChannel = HW_SPI_CHANNEL_ENC_B, .reverse = true };

    config = (IO_AS5048_config_S){ .channels = channelCfg, .numChannels = IO_AS5048_CHANNEL_COUNT };
}

void setUp(void)
{
    mock_HW_SPI_reset();
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- uninitialized-state checks (must precede any successful init, since
        the driver keeps static config and has no reset hook) ---- */

// [test->fw~est_encoder_003~1]
static void test_sampling_before_init_is_noop(void)
{
    IO_AS5048_run1ms();
    uint16_t raw = 0U;
    TEST_ASSERT_FALSE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &raw, NULL));
}

// [test->fw~est_encoder_004~1]
static void test_readout_before_init_fails(void)
{
    uint16_t  raw = 0U;
    float32_t deg = 0.0f;
    TEST_ASSERT_FALSE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &raw, &deg));
}

// [test->fw~est_encoder_006~1]
static void test_status_before_init_fails(void)
{
    IO_AS5048_status_E status = IO_AS5048_STATUS_OK;
    TEST_ASSERT_FALSE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_ENC_A, &status));
}

/* ---- fw~est_encoder_001: init + config validation ---- */

// [test->fw~est_encoder_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
}

// [test->fw~est_encoder_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_AS5048_init(NULL));
}

// [test->fw~est_encoder_001~1]
static void test_init_null_channels(void)
{
    config.channels = NULL;
    TEST_ASSERT_FALSE(IO_AS5048_init(&config));
}

// [test->fw~est_encoder_001~1]
static void test_init_too_many_channels(void)
{
    config.numChannels = IO_AS5048_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_AS5048_init(&config));
}

// [test->fw~est_encoder_001~1]
static void test_init_rejects_out_of_range_spi_channel(void)
{
    channelCfg[IO_AS5048_CHANNEL_ENC_A].spiChannel = HW_SPI_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(IO_AS5048_init(&config));
}

/* ---- fw~est_encoder_002: channel addressing ---- */

// [test->fw~est_encoder_002~1]
static void test_channels_addressed_independently(void)
{
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(1000U, false, true));
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_B, makeFrame(2000U, false, true));

    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
    IO_AS5048_run1ms();

    uint16_t rawA = 0U;
    uint16_t rawB = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &rawA, NULL));
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_B, &rawB, NULL));

    // ENC_A is forward (1000); ENC_B is reverse (16384 - 2000 = 14384).
    TEST_ASSERT_EQUAL_UINT16(1000U, rawA);
    TEST_ASSERT_EQUAL_UINT16(14384U, rawB);

    // Each channel's command went out on its own SPI channel.
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, mock_HW_SPI_lastCommand(HW_SPI_CHANNEL_ENC_A));
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, mock_HW_SPI_lastCommand(HW_SPI_CHANNEL_ENC_B));
}

/* ---- fw~est_encoder_003: polled sampling ---- */

// [test->fw~est_encoder_003~1]
static void test_sampling_updates_stored_angle(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));

    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(100U, false, true));
    IO_AS5048_run1ms();
    uint16_t first = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &first, NULL));
    TEST_ASSERT_EQUAL_UINT16(100U, first);

    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(200U, false, true));
    IO_AS5048_run1ms();
    uint16_t second = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &second, NULL));
    TEST_ASSERT_EQUAL_UINT16(200U, second);
}

/* ---- fw~est_encoder_004: count + degrees readout ---- */

// [test->fw~est_encoder_004~1]
static void test_readout_count_and_degrees(void)
{
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(6844U, false, true));
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
    IO_AS5048_run1ms();

    uint16_t  raw = 0U;
    float32_t deg = 0.0f;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &raw, &deg));
    TEST_ASSERT_EQUAL_UINT16(6844U, raw);

    const float32_t expected = ((float32_t)6844U * 360.0f) / 16384.0f;
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, expected, deg);
}

// [test->fw~est_encoder_004~1]
static void test_readout_out_of_range_fails(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
    uint16_t raw = 0U;
    TEST_ASSERT_FALSE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_COUNT, &raw, NULL));
}

/* ---- fw~est_encoder_005: per-channel reverse ---- */

// [test->fw~est_encoder_005~1]
static void test_reverse_inverts_angle(void)
{
    // Same physical reading injected on both channels; ENC_A forward,
    // ENC_B reverse.
    const uint16_t physical = 3000U;
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(physical, false, true));
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_B, makeFrame(physical, false, true));

    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
    IO_AS5048_run1ms();

    uint16_t fwd = 0U;
    uint16_t rev = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &fwd, NULL));
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_B, &rev, NULL));

    TEST_ASSERT_EQUAL_UINT16(physical, fwd);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)((16384U - physical) % 16384U), rev);
}

/* ---- fw~est_encoder_006: frame integrity + fault status ---- */

// [test->fw~est_encoder_006~1]
static void test_valid_frame_sets_status_ok(void)
{
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(500U, false, true));
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
    IO_AS5048_run1ms();

    IO_AS5048_status_E status = IO_AS5048_STATUS_IDLE;
    TEST_ASSERT_TRUE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_ENC_A, &status));
    TEST_ASSERT_EQUAL(IO_AS5048_STATUS_OK, status);
}

// [test->fw~est_encoder_006~1]
static void test_parity_error_faults_and_holds_angle(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));

    // Establish a known-good angle.
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(777U, false, true));
    IO_AS5048_run1ms();
    uint16_t good = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &good, NULL));
    TEST_ASSERT_EQUAL_UINT16(777U, good);

    // Corrupt parity: angle held, status FAULT.
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(1234U, false, false));
    IO_AS5048_run1ms();

    uint16_t held = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &held, NULL));
    TEST_ASSERT_EQUAL_UINT16(777U, held);

    IO_AS5048_status_E status = IO_AS5048_STATUS_OK;
    TEST_ASSERT_TRUE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_ENC_A, &status));
    TEST_ASSERT_EQUAL(IO_AS5048_STATUS_FAULT, status);
}

// [test->fw~est_encoder_006~1]
static void test_error_flag_faults_and_holds_angle(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));

    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(888U, false, true));
    IO_AS5048_run1ms();

    // Error flag set (parity still valid): angle held, status FAULT.
    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(1234U, true, true));
    IO_AS5048_run1ms();

    uint16_t held = 0U;
    TEST_ASSERT_TRUE(IO_AS5048_readAngle(IO_AS5048_CHANNEL_ENC_A, &held, NULL));
    TEST_ASSERT_EQUAL_UINT16(888U, held);

    IO_AS5048_status_E status = IO_AS5048_STATUS_OK;
    TEST_ASSERT_TRUE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_ENC_A, &status));
    TEST_ASSERT_EQUAL(IO_AS5048_STATUS_FAULT, status);
}

// [test->fw~est_encoder_006~1]
static void test_spi_failure_faults(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));

    mock_HW_SPI_setResponse(HW_SPI_CHANNEL_ENC_A, makeFrame(999U, false, true));
    mock_HW_SPI_setTransferOk(HW_SPI_CHANNEL_ENC_A, false);
    IO_AS5048_run1ms();

    IO_AS5048_status_E status = IO_AS5048_STATUS_OK;
    TEST_ASSERT_TRUE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_ENC_A, &status));
    TEST_ASSERT_EQUAL(IO_AS5048_STATUS_FAULT, status);
}

// [test->fw~est_encoder_006~1]
static void test_status_out_of_range_and_null_fail(void)
{
    TEST_ASSERT_TRUE(IO_AS5048_init(&config));
    IO_AS5048_status_E status = IO_AS5048_STATUS_OK;
    TEST_ASSERT_FALSE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_COUNT, &status));
    TEST_ASSERT_FALSE(IO_AS5048_getStatus(IO_AS5048_CHANNEL_ENC_A, NULL));
}

int main(void)
{
    UNITY_BEGIN();

    // Uninitialized-state checks first.
    RUN_TEST(test_sampling_before_init_is_noop);
    RUN_TEST(test_readout_before_init_fails);
    RUN_TEST(test_status_before_init_fails);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_out_of_range_spi_channel);

    RUN_TEST(test_channels_addressed_independently);

    RUN_TEST(test_sampling_updates_stored_angle);

    RUN_TEST(test_readout_count_and_degrees);
    RUN_TEST(test_readout_out_of_range_fails);

    RUN_TEST(test_reverse_inverts_angle);

    RUN_TEST(test_valid_frame_sets_status_ok);
    RUN_TEST(test_parity_error_faults_and_holds_angle);
    RUN_TEST(test_error_flag_faults_and_holds_angle);
    RUN_TEST(test_spi_failure_faults);
    RUN_TEST(test_status_out_of_range_and_null_fail);

    return UNITY_END();
}
