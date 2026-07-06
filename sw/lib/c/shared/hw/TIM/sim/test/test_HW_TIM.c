#include "HW_TIM.h"
#include "HW_TIM_sim.h"
#include "unity.h"

// Channel 0 is a center/up PWM-style timer with one complementary output;
// channel 1 is a wide free-running time base. Tests tweak the file-scope
// config then call HW_TIM_init, so it must outlive each test.
#define PWM_PERIOD     (1000U)
#define PWM_COMPARE    (400U)
#define PWM_DEADTIME   (50U)

#define PWM_CH         (HW_TIM_CHANNEL_1)
#define BASE_CH        (HW_TIM_CHANNEL_2)
#define OC0            (0U)

static HW_TIM_channelConfig_S timChannels[HW_TIM_CHANNEL_COUNT];
static HW_TIM_config_S        timConfig;

static void buildGoodConfig(void)
{
    for (size_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
    {
        timChannels[ch] = (HW_TIM_channelConfig_S){ 0 };
    }

    timChannels[PWM_CH] = (HW_TIM_channelConfig_S){
        .channelNameStr   = "TIM_PWM",
        .prescaler        = 0U,
        .period           = PWM_PERIOD,
        .counterWidthBits = 16U,
        .countDir         = HW_TIM_COUNT_UP,
        .outputCompare[OC0] = {
            .enabled       = true,
            .complementary = true,
            .compare       = PWM_COMPARE,
            .inactiveLevel = 0U },
        .configureBreakDeadTime = true,
        .deadTime               = PWM_DEADTIME,
        .hasBreakInput          = true,
        .configureTrgo          = true,
        .trgoSource             = HW_TIM_TRGO_UPDATE };

    timChannels[BASE_CH] = (HW_TIM_channelConfig_S){
        .channelNameStr   = "TIM_BASE",
        .prescaler        = 143U,
        .period           = 0xFFFFFFFFU,
        .counterWidthBits = 32U,
        .countDir         = HW_TIM_COUNT_UP };

    timConfig = (HW_TIM_config_S){
        .channels = timChannels, .numChannels = HW_TIM_CHANNEL_COUNT };
}

void setUp(void)
{
    HW_TIM_sim_reset();
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- fw~hal_tim_001: init + config validation ---- */
// [test->fw~hal_tim_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_TIM_init(NULL));
}

// [test->fw~hal_tim_001~1]
static void test_init_null_channels(void)
{
    timConfig.channels = NULL;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_bad_count_direction(void)
{
    timChannels[PWM_CH].countDir = (HW_TIM_countDir_E)99;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_compare_above_period(void)
{
    timChannels[PWM_CH].outputCompare[OC0].compare = PWM_PERIOD + 1U;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_period_above_width(void)
{
    timChannels[PWM_CH].period = 0x10000U;   // exceeds 16-bit width
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_oversized_deadtime(void)
{
    timChannels[PWM_CH].deadTime = 300U;     // dead-time generator is 8 bits
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

/* ---- fw~hal_tim_002: counter direction and period ---- */
// [test->fw~hal_tim_002~1]
static void test_count_up_and_wrap(void)
{
    timChannels[PWM_CH].period = 3U;
    timChannels[PWM_CH].outputCompare[OC0].compare = 0U;   // keep compare <= period
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    uint32_t counter = 0xFFU;
    HW_TIM_sim_advance(PWM_CH, 2U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_CH, &counter));
    TEST_ASSERT_EQUAL_UINT32(2U, counter);

    HW_TIM_sim_advance(PWM_CH, 2U);          // 3 then wrap to 0
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_CH, &counter));
    TEST_ASSERT_EQUAL_UINT32(0U, counter);
}

// [test->fw~hal_tim_002~1]
static void test_count_down(void)
{
    timChannels[PWM_CH].countDir = HW_TIM_COUNT_DOWN;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));   // counter seeds at period

    uint32_t counter = 0U;
    HW_TIM_sim_advance(PWM_CH, 3U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_CH, &counter));
    TEST_ASSERT_EQUAL_UINT32(PWM_PERIOD - 3U, counter);
}

// [test->fw~hal_tim_002~1]
static void test_count_center_triangle(void)
{
    timChannels[PWM_CH].countDir = HW_TIM_COUNT_CENTER;
    timChannels[PWM_CH].period   = 4U;
    timChannels[PWM_CH].outputCompare[OC0].compare = 0U;   // keep compare <= period
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    uint32_t counter = 0U;
    HW_TIM_sim_advance(PWM_CH, 4U);              // ramp up to the top
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_CH, &counter));
    TEST_ASSERT_EQUAL_UINT32(4U, counter);

    HW_TIM_sim_advance(PWM_CH, 4U);              // ramp back down to zero
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_CH, &counter));
    TEST_ASSERT_EQUAL_UINT32(0U, counter);
}

/* ---- fw~hal_tim_003: free-running counter readout ---- */
// [test->fw~hal_tim_003~1]
static void test_getCounter_before_init(void)
{
    uint32_t counter = 0U;
    TEST_ASSERT_FALSE(HW_TIM_getCounter(PWM_CH, &counter));
}

// [test->fw~hal_tim_003~1]
static void test_getCounter_out_of_range(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    uint32_t counter = 0U;
    TEST_ASSERT_FALSE(HW_TIM_getCounter(HW_TIM_CHANNEL_COUNT, &counter));
}

// [test->fw~hal_tim_003~1]
static void test_getCounter_null_out(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_FALSE(HW_TIM_getCounter(PWM_CH, NULL));
}

/* ---- fw~hal_tim_004: output-compare unit operation ---- */
// [test->fw~hal_tim_004~1]
static void test_setCompare_readback(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setCompare(PWM_CH, OC0, 250U));

    uint32_t compare = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCompare(PWM_CH, OC0, &compare));
    TEST_ASSERT_EQUAL_UINT32(250U, compare);
}

// [test->fw~hal_tim_004~1]
static void test_setCompare_above_period_rejected(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_FALSE(HW_TIM_setCompare(PWM_CH, OC0, PWM_PERIOD + 1U));
}

// [test->fw~hal_tim_004~1]
static void test_setCompare_disabled_unit_rejected(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_FALSE(HW_TIM_setCompare(PWM_CH, 1U, 100U));   // unit 1 not enabled
}

// [test->fw~hal_tim_004~1]
static void test_output_level_follows_compare_and_enable(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    // Disabled output sits at the inactive level regardless of compare.
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, OC0, true));
    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));   // MOE gates the output
    // counter 0 < compare 400 -> active.
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    HW_TIM_sim_advance(PWM_CH, 500U);            // counter 500 > compare 400
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
}

/* ---- fw~hal_tim_005: complementary outputs + dead-time ---- */
// [test->fw~hal_tim_005~1]
static void test_complementary_antiphase(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, OC0, true));
    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));   // MOE gates the outputs

    // counter 0 < compare -> primary active, complement inactive.
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getComplementaryLevel(PWM_CH, OC0));

    HW_TIM_sim_advance(PWM_CH, 500U);            // counter past compare -> swap
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getComplementaryLevel(PWM_CH, OC0));
}

// [test->fw~hal_tim_005~1]
static void test_deadtime_configured(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_EQUAL_UINT32(PWM_DEADTIME, HW_TIM_sim_getDeadTime(PWM_CH));
}

/* ---- fw~hal_tim_006: trigger output ---- */
// [test->fw~hal_tim_006~1]
static void test_trgo_on_update(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));   // trgoSource = UPDATE
    HW_TIM_sim_clearTriggers(PWM_CH);

    HW_TIM_sim_advance(PWM_CH, PWM_PERIOD + 1U); // one full period -> one wrap
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getTriggerCount(PWM_CH));
}

// [test->fw~hal_tim_006~1]
static void test_trgo_on_oc_match(void)
{
    timChannels[PWM_CH].trgoSource = HW_TIM_TRGO_OC_MATCH;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    HW_TIM_sim_clearTriggers(PWM_CH);

    HW_TIM_sim_advance(PWM_CH, PWM_PERIOD + 1U); // counter crosses compare once
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getTriggerCount(PWM_CH));
}

/* ---- fw~hal_tim_007: break-input safe-state shutdown ---- */
// [test->fw~hal_tim_007~1]
static void test_break_forces_outputs_inactive(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, OC0, true));
    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    HW_TIM_sim_assertBreak(PWM_CH, true);
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getComplementaryLevel(PWM_CH, OC0));

    // Break release does not restore outputs: they stay inactive until the
    // master output enable is set again (fw~hal_tim_008).
    HW_TIM_sim_assertBreak(PWM_CH, false);
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
}

/* ---- fw~hal_tim_008: master output enable ---- */

// Bullet 1: clearing MOE holds every enabled output at its inactive state.
// [test->fw~hal_tim_008~1]
static void test_moe_gates_enabled_output(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, OC0, true));

    // MOE is commanded OFF at init, so an enabled unit still reads inactive.
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, false));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getComplementaryLevel(PWM_CH, OC0));
}

// Bullet 2: setting MOE restores outputs per the unchanged per-unit config;
// the clear leaves compare values and enables intact. The reported state
// tracks the commanded state with no break asserted.
// [test->fw~hal_tim_008~1]
static void test_moe_restore_preserves_config(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, OC0, true));
    TEST_ASSERT_TRUE(HW_TIM_setCompare(PWM_CH, OC0, 250U));
    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));

    bool moe = false;
    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_CH, &moe));
    TEST_ASSERT_TRUE(moe);
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, false));
    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_CH, &moe));
    TEST_ASSERT_FALSE(moe);

    // Per-unit compare survived the gate toggling.
    uint32_t compare = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCompare(PWM_CH, OC0, &compare));
    TEST_ASSERT_EQUAL_UINT32(250U, compare);

    // Re-enabling restores the waveform: counter 0 < compare 250 -> active.
    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
}

// Bullet 3: after a break assertion and release, the reported state reads
// disabled and outputs stay inactive until MOE is set again.
// [test->fw~hal_tim_008~1]
static void test_moe_reads_disabled_after_break(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, OC0, true));
    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));

    bool moe = false;
    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_CH, &moe));
    TEST_ASSERT_TRUE(moe);

    HW_TIM_sim_assertBreak(PWM_CH, true);
    HW_TIM_sim_assertBreak(PWM_CH, false);

    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_CH, &moe));
    TEST_ASSERT_FALSE(moe);
    TEST_ASSERT_EQUAL_UINT32(0U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_CH, true));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_TIM_sim_getOutputLevel(PWM_CH, OC0));
}

// Bullet 4: set, clear, and read on an out-of-range or uninitialized channel
// return false.
// [test->fw~hal_tim_008~1]
static void test_moe_error_returns(void)
{
    bool moe = false;

    // Uninitialized driver.
    TEST_ASSERT_FALSE(HW_TIM_setMainOutputEnabled(PWM_CH, true));
    TEST_ASSERT_FALSE(HW_TIM_getMainOutputEnabled(PWM_CH, &moe));

    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    // Out-of-range channel.
    TEST_ASSERT_FALSE(HW_TIM_setMainOutputEnabled(HW_TIM_CHANNEL_COUNT, true));
    TEST_ASSERT_FALSE(HW_TIM_getMainOutputEnabled(HW_TIM_CHANNEL_COUNT, &moe));

    // NULL out-pointer.
    TEST_ASSERT_FALSE(HW_TIM_getMainOutputEnabled(PWM_CH, NULL));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_rejects_bad_count_direction);
    RUN_TEST(test_init_rejects_compare_above_period);
    RUN_TEST(test_init_rejects_period_above_width);
    RUN_TEST(test_init_rejects_oversized_deadtime);

    RUN_TEST(test_count_up_and_wrap);
    RUN_TEST(test_count_down);
    RUN_TEST(test_count_center_triangle);

    RUN_TEST(test_getCounter_before_init);
    RUN_TEST(test_getCounter_out_of_range);
    RUN_TEST(test_getCounter_null_out);

    RUN_TEST(test_setCompare_readback);
    RUN_TEST(test_setCompare_above_period_rejected);
    RUN_TEST(test_setCompare_disabled_unit_rejected);
    RUN_TEST(test_output_level_follows_compare_and_enable);

    RUN_TEST(test_complementary_antiphase);
    RUN_TEST(test_deadtime_configured);

    RUN_TEST(test_trgo_on_update);
    RUN_TEST(test_trgo_on_oc_match);

    RUN_TEST(test_break_forces_outputs_inactive);

    RUN_TEST(test_moe_gates_enabled_output);
    RUN_TEST(test_moe_restore_preserves_config);
    RUN_TEST(test_moe_reads_disabled_after_break);
    RUN_TEST(test_moe_error_returns);

    return UNITY_END();
}
