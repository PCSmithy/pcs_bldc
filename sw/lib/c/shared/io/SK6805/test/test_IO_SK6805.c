#include "IO_SK6805.h"
#include "mock_HW_SPI.h"
#include "unity.h"

// File-scope config the tests build and tweak; IO_SK6805_init stores a pointer
// to it, so it must outlive each test.
static IO_SK6805_channelConfig_S channelCfg[IO_SK6805_CHANNEL_COUNT];
static IO_SK6805_config_S        config;

// The driver's 6-SPI-bit code for a set LED bit (the other code is for a clear
// bit). Mirrors the encoder so the test can decode a captured frame.
#define SK6805_CODE_1_WIRE  0x38U

// Decode the 6 SPI bytes of one colour byte's expansion (non-inverted) back to
// its 8-bit value: 8 codes of 6 bits, MSB first.
static uint8_t decodeColour(const uint8_t * six)
{
    uint64_t acc = 0U;
    for (uint8_t b = 0U; b < 6U; b++) { acc = (acc << 8U) | (uint64_t)six[b]; }

    uint8_t value = 0U;
    for (uint8_t i = 0U; i < 8U; i++)
    {
        const uint8_t code = (uint8_t)((acc >> (42U - (i * 6U))) & 0x3FU);
        value = (uint8_t)((value << 1U) | ((code == SK6805_CODE_1_WIRE) ? 1U : 0U));
    }
    return value;
}

// Decode pixel `px`'s colour from a non-inverted transmit buffer (GRB wire
// order: 6 bytes green, 6 red, 6 blue).
static void decodePixel(const uint8_t * tx, uint16_t px, uint8_t * red, uint8_t * green, uint8_t * blue)
{
    const size_t base = (size_t)px * 3U * 6U;
    *green = decodeColour(&tx[base + 0U]);
    *red   = decodeColour(&tx[base + 6U]);
    *blue  = decodeColour(&tx[base + 12U]);
}

// Two non-inverting channels on distinct SPI channels.
static void buildGoodConfig(void)
{
    channelCfg[IO_SK6805_CHANNEL_A] =
        (IO_SK6805_channelConfig_S){ .spiChannel = HW_SPI_CHANNEL_LED_A, .invert = false };
    channelCfg[IO_SK6805_CHANNEL_B] =
        (IO_SK6805_channelConfig_S){ .spiChannel = HW_SPI_CHANNEL_LED_B, .invert = false };

    config = (IO_SK6805_config_S){ .channels = channelCfg, .numChannels = IO_SK6805_CHANNEL_COUNT };
}

void setUp(void)
{
    mock_HW_SPI_reset();
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- uninitialized-state check (precedes any init; driver keeps static
        config and has no reset hook) ---- */

// [test->fw~obs_led_004~1]
static void test_update_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_SK6805_update(IO_SK6805_CHANNEL_A));
}

/* ---- fw~obs_led_001: init + config validation ---- */

// [test->fw~obs_led_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));
}

// [test->fw~obs_led_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_SK6805_init(NULL));
}

// [test->fw~obs_led_001~1]
static void test_init_null_channels(void)
{
    config.channels = NULL;
    TEST_ASSERT_FALSE(IO_SK6805_init(&config));
}

// [test->fw~obs_led_001~1]
static void test_init_too_many_channels(void)
{
    config.numChannels = IO_SK6805_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_SK6805_init(&config));
}

// [test->fw~obs_led_001~1]
static void test_init_rejects_out_of_range_spi_channel(void)
{
    channelCfg[IO_SK6805_CHANNEL_A].spiChannel = HW_SPI_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(IO_SK6805_init(&config));
}

/* ---- fw~obs_led_002: channel addressing ---- */

// [test->fw~obs_led_002~1]
static void test_channels_addressed_independently(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));

    IO_SK6805_setAll(IO_SK6805_CHANNEL_A, 10U, 20U, 30U);
    IO_SK6805_setAll(IO_SK6805_CHANNEL_B, 40U, 50U, 60U);
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_B));

    // Each channel transmitted its own colour on its own SPI channel.
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), 0U, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(10U, r);
    TEST_ASSERT_EQUAL_UINT8(20U, g);
    TEST_ASSERT_EQUAL_UINT8(30U, b);

    decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_B), 0U, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(40U, r);
    TEST_ASSERT_EQUAL_UINT8(50U, g);
    TEST_ASSERT_EQUAL_UINT8(60U, b);
}

/* ---- fw~obs_led_003: per-pixel framebuffer ---- */

// [test->fw~obs_led_003~1]
static void test_setpixel_latest_wins(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));
    IO_SK6805_clear(IO_SK6805_CHANNEL_A);

    IO_SK6805_setPixel(IO_SK6805_CHANNEL_A, 1U, 1U, 2U, 3U);
    IO_SK6805_setPixel(IO_SK6805_CHANNEL_A, 1U, 7U, 8U, 9U);   // overwrite
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));

    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), 1U, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(7U, r);
    TEST_ASSERT_EQUAL_UINT8(8U, g);
    TEST_ASSERT_EQUAL_UINT8(9U, b);

    // An untouched pixel stays off.
    decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), 0U, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(0U, r | g | b);
}

// [test->fw~obs_led_003~1]
static void test_setall_and_clear(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));

    IO_SK6805_setAll(IO_SK6805_CHANNEL_A, 11U, 22U, 33U);
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));
    for (uint16_t px = 0U; px < IO_SK6805_PIXEL_COUNT; px++)
    {
        uint8_t r = 0U;
        uint8_t g = 0U;
        uint8_t b = 0U;
        decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), px, &r, &g, &b);
        TEST_ASSERT_EQUAL_UINT8(11U, r);
        TEST_ASSERT_EQUAL_UINT8(22U, g);
        TEST_ASSERT_EQUAL_UINT8(33U, b);
    }

    IO_SK6805_clear(IO_SK6805_CHANNEL_A);
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));
    for (uint16_t px = 0U; px < IO_SK6805_PIXEL_COUNT; px++)
    {
        uint8_t r = 0U;
        uint8_t g = 0U;
        uint8_t b = 0U;
        decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), px, &r, &g, &b);
        TEST_ASSERT_EQUAL_UINT8(0U, r | g | b);
    }
}

// [test->fw~obs_led_003~1]
static void test_out_of_range_pixel_ignored(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));
    IO_SK6805_setAll(IO_SK6805_CHANNEL_A, 5U, 6U, 7U);
    IO_SK6805_setPixel(IO_SK6805_CHANNEL_A, IO_SK6805_PIXEL_COUNT, 99U, 99U, 99U);  // out of range
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));

    for (uint16_t px = 0U; px < IO_SK6805_PIXEL_COUNT; px++)
    {
        uint8_t r = 0U;
        uint8_t g = 0U;
        uint8_t b = 0U;
        decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), px, &r, &g, &b);
        TEST_ASSERT_EQUAL_UINT8(5U, r);
        TEST_ASSERT_EQUAL_UINT8(6U, g);
        TEST_ASSERT_EQUAL_UINT8(7U, b);
    }
}

/* ---- fw~obs_led_004: frame transmission ---- */

// [test->fw~obs_led_004~1]
static void test_transmit_emits_stored_colour_unmodified(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));
    IO_SK6805_clear(IO_SK6805_CHANNEL_A);
    IO_SK6805_setPixel(IO_SK6805_CHANNEL_A, 2U, 200U, 100U, 50U);
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));

    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    decodePixel(mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A), 2U, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(200U, r);
    TEST_ASSERT_EQUAL_UINT8(100U, g);
    TEST_ASSERT_EQUAL_UINT8(50U, b);
}

// [test->fw~obs_led_004~1]
static void test_update_transfer_failure_returns_false(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));
    mock_HW_SPI_setTransferOk(HW_SPI_CHANNEL_LED_A, false);
    TEST_ASSERT_FALSE(IO_SK6805_update(IO_SK6805_CHANNEL_A));
}

// [test->fw~obs_led_004~1]
static void test_update_out_of_range_channel_returns_false(void)
{
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));
    TEST_ASSERT_FALSE(IO_SK6805_update(IO_SK6805_CHANNEL_COUNT));
}

/* ---- fw~obs_led_005: inverting-output polarity ---- */

// [test->fw~obs_led_005~1]
static void test_invert_complements_entire_signal(void)
{
    // Channel A non-inverting, channel B inverting; same colour on both.
    channelCfg[IO_SK6805_CHANNEL_B].invert = true;
    TEST_ASSERT_TRUE(IO_SK6805_init(&config));

    IO_SK6805_setAll(IO_SK6805_CHANNEL_A, 123U, 45U, 67U);
    IO_SK6805_setAll(IO_SK6805_CHANNEL_B, 123U, 45U, 67U);
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_A));
    TEST_ASSERT_TRUE(IO_SK6805_update(IO_SK6805_CHANNEL_B));

    const uint8_t * direct   = mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_A);
    const uint8_t * inverted = mock_HW_SPI_txBuf(HW_SPI_CHANNEL_LED_B);
    const size_t    len      = mock_HW_SPI_txLen(HW_SPI_CHANNEL_LED_A);

    TEST_ASSERT_EQUAL_size_t(len, mock_HW_SPI_txLen(HW_SPI_CHANNEL_LED_B));
    TEST_ASSERT_TRUE(len > 0U);

    // Entire signal (data + reset gap) is the bitwise complement.
    for (size_t i = 0U; i < len; i++)
    {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(~direct[i]), inverted[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_update_before_init_fails);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_out_of_range_spi_channel);

    RUN_TEST(test_channels_addressed_independently);

    RUN_TEST(test_setpixel_latest_wins);
    RUN_TEST(test_setall_and_clear);
    RUN_TEST(test_out_of_range_pixel_ignored);

    RUN_TEST(test_transmit_emits_stored_colour_unmodified);
    RUN_TEST(test_update_transfer_failure_returns_false);
    RUN_TEST(test_update_out_of_range_channel_returns_false);

    RUN_TEST(test_invert_complements_entire_signal);

    return UNITY_END();
}
