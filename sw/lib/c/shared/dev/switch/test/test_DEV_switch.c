#include "DEV_switch.h"
#include "mock_HW_GPIO.h"
#include "unity.h"

// Unit tests for the debounced switch driver. These are untagged: DEV_switch
// has no fw~ spec yet, so there is no requirement ID to trace to. Add
// [test->...] tags once a dev-switch spec is back-filled.

// Two buttons with distinct ports/pins, debounce thresholds, and active-level
// polarities. BTN_A is active-low (idle high), BTN_B active-high (idle low).
#define BTN_A_PORT      HW_GPIO_PORT_A
#define BTN_A_PIN       (0x0001U)
#define BTN_A_DEBOUNCE  (3U)
#define BTN_B_PORT      HW_GPIO_PORT_B
#define BTN_B_PIN       (0x0002U)
#define BTN_B_DEBOUNCE  (2U)

// File-scope config the tests build and tweak; DEV_switch_init stores a pointer
// to it, so it must outlive each test.
static DEV_switch_channelConfig_S channelCfg[DEV_SWITCH_CHANNEL_COUNT];
static DEV_switch_config_S        config;

static void buildGoodConfig(void)
{
    channelCfg[DEV_SWITCH_CHANNEL_BTN_A] = (DEV_switch_channelConfig_S){
        .port = BTN_A_PORT, .pin = BTN_A_PIN,
        .activeLevel = HW_GPIO_LEVEL_LOW, .debounceCount = BTN_A_DEBOUNCE };
    channelCfg[DEV_SWITCH_CHANNEL_BTN_B] = (DEV_switch_channelConfig_S){
        .port = BTN_B_PORT, .pin = BTN_B_PIN,
        .activeLevel = HW_GPIO_LEVEL_HIGH, .debounceCount = BTN_B_DEBOUNCE };

    config = (DEV_switch_config_S){ .channels = channelCfg, .numChannels = DEV_SWITCH_CHANNEL_COUNT };
}

// Pump the periodic debounce update `n` times.
static void pump(uint16_t n)
{
    for (uint16_t i = 0U; i < n; i++)
    {
        DEV_switch_run1ms();
    }
}

void setUp(void)
{
    mock_HW_GPIO_reset();
    buildGoodConfig();
    // Rest both buttons at their inactive level.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_HIGH);  // active-low -> idle high
    mock_HW_GPIO_setCachedLevel(BTN_B_PORT, BTN_B_PIN, HW_GPIO_LEVEL_LOW);   // active-high -> idle low
}

void tearDown(void) {}

/* ---- uninitialized-state checks (must precede any successful init, since the
        driver keeps static config and has no reset hook) ---- */

static void test_isActive_before_init_false(void)
{
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_run1ms_before_init_is_noop(void)
{
    // Hold the button at its active level, but with no init the update does
    // nothing and the channel never reads active.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    pump(BTN_A_DEBOUNCE + 2U);
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

/* ---- init + config validation ---- */

static void test_init_null_config_false(void)
{
    TEST_ASSERT_FALSE(DEV_switch_init(NULL));
}

static void test_init_valid_config_true(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));
}

static void test_init_rejects_out_of_range_port(void)
{
    channelCfg[DEV_SWITCH_CHANNEL_BTN_A].port = HW_GPIO_PORT_COUNT;
    TEST_ASSERT_FALSE(DEV_switch_init(&config));
}

/* ---- debounce behaviour ---- */

static void test_press_registers_after_debounce_count(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));

    // Drive the active level; the state flips only on the debounceCount-th
    // consecutive sample.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    pump(BTN_A_DEBOUNCE - 1U);
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
    pump(1U);
    TEST_ASSERT_TRUE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_release_registers_after_debounce_count(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));

    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    pump(BTN_A_DEBOUNCE);
    TEST_ASSERT_TRUE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));

    // Release: back to the inactive level, again debounced.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_HIGH);
    pump(BTN_A_DEBOUNCE - 1U);
    TEST_ASSERT_TRUE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
    pump(1U);
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_bounce_shorter_than_debounce_is_rejected(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));

    // A transient that lasts fewer than debounceCount samples and then returns
    // to the resting level never flips the state.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    pump(BTN_A_DEBOUNCE - 1U);
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_HIGH);
    pump(BTN_A_DEBOUNCE);
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_active_high_polarity(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));

    // BTN_B is active-high: driving it high registers a press.
    mock_HW_GPIO_setCachedLevel(BTN_B_PORT, BTN_B_PIN, HW_GPIO_LEVEL_HIGH);
    pump(BTN_B_DEBOUNCE - 1U);
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_B));
    pump(1U);
    TEST_ASSERT_TRUE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_B));
}

static void test_channels_debounce_independently(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));

    // Press BTN_A fully; BTN_B, left at its resting level, stays inactive.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    pump(BTN_A_DEBOUNCE);
    TEST_ASSERT_TRUE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_BTN_B));
}

static void test_isActive_out_of_range_channel_false(void)
{
    TEST_ASSERT_TRUE(DEV_switch_init(&config));
    TEST_ASSERT_FALSE(DEV_switch_isActive(DEV_SWITCH_CHANNEL_COUNT));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_isActive_before_init_false);
    RUN_TEST(test_run1ms_before_init_is_noop);

    RUN_TEST(test_init_null_config_false);
    RUN_TEST(test_init_valid_config_true);
    RUN_TEST(test_init_rejects_out_of_range_port);

    RUN_TEST(test_press_registers_after_debounce_count);
    RUN_TEST(test_release_registers_after_debounce_count);
    RUN_TEST(test_bounce_shorter_than_debounce_is_rejected);
    RUN_TEST(test_active_high_polarity);
    RUN_TEST(test_channels_debounce_independently);
    RUN_TEST(test_isActive_out_of_range_channel_false);

    return UNITY_END();
}
