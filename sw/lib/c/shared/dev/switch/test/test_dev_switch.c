#include "dev_switch.h"
#include "mock_HW_GPIO.h"
#include "lib_timer.h"
#include "unity.h"

// Unit tests for the debounced switch driver. These are untagged: dev_switch
// has no fw~ spec yet, so there is no requirement ID to trace to. Add
// [test->...] tags once a dev-switch spec is back-filled.

// ---------------------------------------------------------------------------
// Fake time source. dev_switch debounces via lib_timer, which binds its time
// base to `lib_timer_config` at static init; the test supplies that definition
// as a settable "HW counter" and advances it one millisecond per simulated
// 1 ms task tick (see tick()).
// ---------------------------------------------------------------------------
static uint32_t fakeCounter_us;

static uint32_t test_getTime_us(void)
{
    return fakeCounter_us;
}

const lib_timer_config_S lib_timer_config =
{
    .getTime_us = test_getTime_us,
};

#define DEBOUNCE_MS  20U

// BTN_A: HW digital-in, active-low (idle HIGH, pressed LOW).
// BTN_B: HW digital-in, active-high (idle LOW, pressed HIGH).
#define BTN_A_PORT   HW_GPIO_PORT_A
#define BTN_A_PIN    (0x0001U)
#define BTN_B_PORT   HW_GPIO_PORT_B
#define BTN_B_PIN    (0x0002U)

// Network switch state, driven directly by the tests via its getState callback.
static dev_switch_state_E netState;

static dev_switch_state_E test_netGetState(void)
{
    return netState;
}

// File-scope config the tests build and tweak; dev_switch_init stores a pointer
// to it, so it must outlive each test.
static dev_switch_channelConfig_S channelCfg[DEV_SWITCH_CHANNEL_COUNT];
static dev_switch_config_S        config;

static void buildGoodConfig(void)
{
    channelCfg[DEV_SWITCH_CHANNEL_BTN_A] = (dev_switch_channelConfig_S){
        .type = DEV_SWITCH_TYPE_HW_DIGIN,
        .hwDigIn = { .port = BTN_A_PORT, .pin = BTN_A_PIN, .activeLevel = HW_GPIO_LEVEL_LOW },
        .debounce_ms = DEBOUNCE_MS };
    channelCfg[DEV_SWITCH_CHANNEL_BTN_B] = (dev_switch_channelConfig_S){
        .type = DEV_SWITCH_TYPE_HW_DIGIN,
        .hwDigIn = { .port = BTN_B_PORT, .pin = BTN_B_PIN, .activeLevel = HW_GPIO_LEVEL_HIGH },
        .debounce_ms = DEBOUNCE_MS };
    channelCfg[DEV_SWITCH_CHANNEL_BTN_NET] = (dev_switch_channelConfig_S){
        .type = DEV_SWITCH_TYPE_NETWORK,
        .network = { .getState = test_netGetState },
        .debounce_ms = DEBOUNCE_MS };

    config = (dev_switch_config_S){ .channels = channelCfg, .numChannels = DEV_SWITCH_CHANNEL_COUNT };
}

// Advance the fake clock and run the debounce update once per millisecond, as
// the real 1 ms task would. Debounce is wall-clock based, so a change latches
// after its input has held steady for more than debounce_ms.
static void tick(uint32_t ms)
{
    for (uint32_t i = 0U; i < ms; i++)
    {
        fakeCounter_us += 1000U;
        dev_switch_run1ms();
    }
}

// Init and let every channel settle from UNKNOWN to its resting INACTIVE state.
static void initAndSettle(void)
{
    TEST_ASSERT_TRUE(dev_switch_init(&config));
    tick(DEBOUNCE_MS + 5U);
}

void setUp(void)
{
    mock_HW_GPIO_reset();
    buildGoodConfig();
    // Rest each switch at its inactive level.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_HIGH);  // active-low -> idle high
    mock_HW_GPIO_setCachedLevel(BTN_B_PORT, BTN_B_PIN, HW_GPIO_LEVEL_LOW);   // active-high -> idle low
    netState = DEV_SWITCH_STATE_INACTIVE;
}

void tearDown(void) {}

/* ---- uninitialized-state checks (must precede any successful init, since the
        driver keeps static config and has no de-init hook) ---- */

static void test_isActive_before_init_false(void)
{
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_run1ms_before_init_is_noop(void)
{
    // Hold the button active, but with no init the update does nothing and the
    // channel never reads active.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    tick(DEBOUNCE_MS + 5U);
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

/* ---- init + config validation ---- */

static void test_init_null_config_false(void)
{
    TEST_ASSERT_FALSE(dev_switch_init(NULL));
}

static void test_init_valid_config_true(void)
{
    TEST_ASSERT_TRUE(dev_switch_init(&config));
}

static void test_init_rejects_out_of_range_port(void)
{
    channelCfg[DEV_SWITCH_CHANNEL_BTN_A].hwDigIn.port = HW_GPIO_PORT_COUNT;
    TEST_ASSERT_FALSE(dev_switch_init(&config));
}

static void test_init_rejects_network_without_getState(void)
{
    channelCfg[DEV_SWITCH_CHANNEL_BTN_NET].network.getState = NULL;
    TEST_ASSERT_FALSE(dev_switch_init(&config));
}

/* ---- state model ---- */

static void test_state_unknown_before_settle(void)
{
    TEST_ASSERT_TRUE(dev_switch_init(&config));
    TEST_ASSERT_EQUAL_INT(DEV_SWITCH_STATE_UNKNOWN, dev_switch_getState(DEV_SWITCH_CHANNEL_BTN_A));
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_settles_to_inactive_at_rest(void)
{
    initAndSettle();
    TEST_ASSERT_EQUAL_INT(DEV_SWITCH_STATE_INACTIVE, dev_switch_getState(DEV_SWITCH_CHANNEL_BTN_A));
}

/* ---- debounce behaviour (HW digital-in) ---- */

static void test_press_registers_after_debounce(void)
{
    initAndSettle();

    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    tick(DEBOUNCE_MS / 2U);   // held less than the debounce window
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));

    tick(DEBOUNCE_MS);        // now held past it
    TEST_ASSERT_TRUE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
    TEST_ASSERT_EQUAL_INT(DEV_SWITCH_STATE_ACTIVE, dev_switch_getState(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_release_registers_after_debounce(void)
{
    initAndSettle();

    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    tick(DEBOUNCE_MS + 5U);
    TEST_ASSERT_TRUE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));

    // Release: back to the inactive level, again debounced.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_HIGH);
    tick(DEBOUNCE_MS / 2U);
    TEST_ASSERT_TRUE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
    tick(DEBOUNCE_MS);
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_bounce_shorter_than_debounce_is_rejected(void)
{
    initAndSettle();

    // A transient that clears before the debounce window elapses never latches.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    tick(DEBOUNCE_MS / 2U);
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_HIGH);
    tick(DEBOUNCE_MS + 5U);
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
}

static void test_active_high_polarity(void)
{
    initAndSettle();

    // BTN_B is active-high: driving it high registers a press.
    mock_HW_GPIO_setCachedLevel(BTN_B_PORT, BTN_B_PIN, HW_GPIO_LEVEL_HIGH);
    tick(DEBOUNCE_MS + 5U);
    TEST_ASSERT_TRUE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_B));
}

/* ---- network-backed switch ---- */

static void test_network_switch_debounces(void)
{
    initAndSettle();

    netState = DEV_SWITCH_STATE_ACTIVE;
    tick(DEBOUNCE_MS / 2U);
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_NET));

    tick(DEBOUNCE_MS);
    TEST_ASSERT_TRUE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_NET));
}

/* ---- addressing ---- */

static void test_channels_debounce_independently(void)
{
    initAndSettle();

    // Press BTN_A fully; BTN_B and BTN_NET, left at rest, stay inactive.
    mock_HW_GPIO_setCachedLevel(BTN_A_PORT, BTN_A_PIN, HW_GPIO_LEVEL_LOW);
    tick(DEBOUNCE_MS + 5U);
    TEST_ASSERT_TRUE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_A));
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_B));
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_BTN_NET));
}

static void test_getState_out_of_range_channel_unknown(void)
{
    TEST_ASSERT_TRUE(dev_switch_init(&config));
    TEST_ASSERT_EQUAL_INT(DEV_SWITCH_STATE_UNKNOWN, dev_switch_getState(DEV_SWITCH_CHANNEL_COUNT));
}

static void test_isActive_out_of_range_channel_false(void)
{
    TEST_ASSERT_TRUE(dev_switch_init(&config));
    TEST_ASSERT_FALSE(dev_switch_isActive(DEV_SWITCH_CHANNEL_COUNT));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_isActive_before_init_false);
    RUN_TEST(test_run1ms_before_init_is_noop);

    RUN_TEST(test_init_null_config_false);
    RUN_TEST(test_init_valid_config_true);
    RUN_TEST(test_init_rejects_out_of_range_port);
    RUN_TEST(test_init_rejects_network_without_getState);

    RUN_TEST(test_state_unknown_before_settle);
    RUN_TEST(test_settles_to_inactive_at_rest);

    RUN_TEST(test_press_registers_after_debounce);
    RUN_TEST(test_release_registers_after_debounce);
    RUN_TEST(test_bounce_shorter_than_debounce_is_rejected);
    RUN_TEST(test_active_high_polarity);
    RUN_TEST(test_network_switch_debounces);
    RUN_TEST(test_channels_debounce_independently);

    RUN_TEST(test_getState_out_of_range_channel_unknown);
    RUN_TEST(test_isActive_out_of_range_channel_false);

    return UNITY_END();
}
