#include "IO_PWM.h"
#include "mock_HW_TIM.h"
#include "unity.h"

// The three phases share HW_TIM_CHANNEL_1, one output-compare unit each, and
// that channel's master output enable gates the whole bridge. A round period
// keeps the duty math exact (half = 2000, full = 4000) without depending on the
// real ARR.
#define TEST_PERIOD   (4000U)
#define BRIDGE_CH     (HW_TIM_CHANNEL_1)

static IO_PWM_phaseConfig_S phaseCfg[IO_PWM_PHASE_COUNT];
static IO_PWM_config_S      config;

static void buildGoodConfig(void)
{
    phaseCfg[IO_PWM_PHASE_U] = (IO_PWM_phaseConfig_S){ .timChannel = HW_TIM_CHANNEL_1, .ocUnit = 0U };
    phaseCfg[IO_PWM_PHASE_V] = (IO_PWM_phaseConfig_S){ .timChannel = HW_TIM_CHANNEL_1, .ocUnit = 1U };
    phaseCfg[IO_PWM_PHASE_W] = (IO_PWM_phaseConfig_S){ .timChannel = HW_TIM_CHANNEL_1, .ocUnit = 2U };

    config = (IO_PWM_config_S){
        .phases = phaseCfg, .numPhases = IO_PWM_PHASE_COUNT, .bridgeTimChannel = BRIDGE_CH };
}

void setUp(void)
{
    mock_HW_TIM_reset(TEST_PERIOD);
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- uninitialized-state checks (must precede any successful init, since the
        driver keeps static config and has no reset hook) ---- */

// [test->fw~io_pwm_002~1]
static void test_setDuty_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_PWM_setDuty(IO_PWM_PHASE_U, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getCompare(BRIDGE_CH, 0U));
}

// [test->fw~io_pwm_003~1]
static void test_setOutputEnabled_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_PWM_setOutputEnabled(true));
    TEST_ASSERT_FALSE(mock_HW_TIM_getMoe(BRIDGE_CH));
}

// [test->fw~io_pwm_003~1]
static void test_getOutputEnabled_before_init_fails(void)
{
    bool enabled = true;
    TEST_ASSERT_FALSE(IO_PWM_getOutputEnabled(&enabled));
}

/* ---- fw~io_pwm_001: init + config validation ---- */

// [test->fw~io_pwm_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));

    // Every phase's output-compare unit is enabled up front...
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_1, 0U));
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_1, 1U));
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_1, 2U));
    // ...but the bridge stays dark: MOE off until IO_PWM_setOutputEnabled.
    TEST_ASSERT_FALSE(mock_HW_TIM_getMoe(BRIDGE_CH));
}

// [test->fw~io_pwm_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_PWM_init(NULL));
}

// [test->fw~io_pwm_001~1]
static void test_init_null_phases(void)
{
    config.phases = NULL;
    TEST_ASSERT_FALSE(IO_PWM_init(&config));
}

// [test->fw~io_pwm_001~1]
static void test_init_too_many_phases(void)
{
    config.numPhases = IO_PWM_PHASE_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_PWM_init(&config));
}

// [test->fw~io_pwm_001~1]
static void test_init_rejects_out_of_range_channel(void)
{
    phaseCfg[IO_PWM_PHASE_V].timChannel = HW_TIM_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(IO_PWM_init(&config));
}

// [test->fw~io_pwm_001~1]
static void test_init_rejects_out_of_range_ocUnit(void)
{
    phaseCfg[IO_PWM_PHASE_W].ocUnit = HW_TIM_OC_UNITS_PER_CHANNEL;
    TEST_ASSERT_FALSE(IO_PWM_init(&config));
}

/* ---- fw~io_pwm_002: per-phase duty command ---- */

// [test->fw~io_pwm_002~1]
static void test_duty_maps_zero_half_full(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));

    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_U, 0.0f));
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));

    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_U, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 2U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));

    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_U, 1.0f));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));
}

// [test->fw~io_pwm_002~1]
static void test_duty_routes_to_each_phase_unit(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));

    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_V, 0.25f));
    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_W, 0.75f));

    // V -> ocUnit 1, W -> ocUnit 2; U (ocUnit 0) untouched.
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 4U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 1U));
    TEST_ASSERT_EQUAL_UINT32((TEST_PERIOD * 3U) / 4U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 2U));
}

// [test->fw~io_pwm_002~1]
static void test_duty_out_of_range_rejected_leaves_compare(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));

    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_U, 0.5f));
    const uint32_t before = mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U);

    TEST_ASSERT_FALSE(IO_PWM_setDuty(IO_PWM_PHASE_U, 1.5f));
    TEST_ASSERT_FALSE(IO_PWM_setDuty(IO_PWM_PHASE_U, -0.1f));
    TEST_ASSERT_EQUAL_UINT32(before, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));
}

// [test->fw~io_pwm_002~1]
static void test_duty_out_of_range_phase_rejected(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));
    TEST_ASSERT_FALSE(IO_PWM_setDuty(IO_PWM_PHASE_COUNT, 0.5f));
}

/* ---- fw~io_pwm_003: bridge output enable ---- */

// [test->fw~io_pwm_003~1]
static void test_output_enable_gates_bridge_and_reports(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));

    bool enabled = true;
    TEST_ASSERT_TRUE(IO_PWM_getOutputEnabled(&enabled));
    TEST_ASSERT_FALSE(enabled);                       // starts disabled

    TEST_ASSERT_TRUE(IO_PWM_setOutputEnabled(true));
    TEST_ASSERT_TRUE(mock_HW_TIM_getMoe(BRIDGE_CH));
    TEST_ASSERT_TRUE(IO_PWM_getOutputEnabled(&enabled));
    TEST_ASSERT_TRUE(enabled);                        // reported matches commanded

    TEST_ASSERT_TRUE(IO_PWM_setOutputEnabled(false));
    TEST_ASSERT_FALSE(mock_HW_TIM_getMoe(BRIDGE_CH)); // outputs held inactive
    TEST_ASSERT_TRUE(IO_PWM_getOutputEnabled(&enabled));
    TEST_ASSERT_FALSE(enabled);
}

// [test->fw~io_pwm_003~1]
static void test_duty_while_disabled_takes_effect_at_reenable(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));
    TEST_ASSERT_TRUE(IO_PWM_setOutputEnabled(false));

    // A duty command while disabled is accepted and lands on the compare
    // register, even though no output is driven yet.
    TEST_ASSERT_TRUE(IO_PWM_setDuty(IO_PWM_PHASE_U, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 2U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));

    // Re-enabling drives that already-staged compare value.
    TEST_ASSERT_TRUE(IO_PWM_setOutputEnabled(true));
    TEST_ASSERT_TRUE(mock_HW_TIM_getMoe(BRIDGE_CH));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 2U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_1, 0U));
}

// [test->fw~io_pwm_003~1]
static void test_reported_disabled_after_break(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));
    TEST_ASSERT_TRUE(IO_PWM_setOutputEnabled(true));

    bool enabled = false;
    TEST_ASSERT_TRUE(IO_PWM_getOutputEnabled(&enabled));
    TEST_ASSERT_TRUE(enabled);

    // A break clears the timer's MOE behind the driver's back.
    mock_HW_TIM_assertBreak(BRIDGE_CH);
    TEST_ASSERT_TRUE(IO_PWM_getOutputEnabled(&enabled));
    TEST_ASSERT_FALSE(enabled);
}

// [test->fw~io_pwm_003~1]
static void test_getOutputEnabled_null_rejected(void)
{
    TEST_ASSERT_TRUE(IO_PWM_init(&config));
    TEST_ASSERT_FALSE(IO_PWM_getOutputEnabled(NULL));
}

int main(void)
{
    UNITY_BEGIN();

    // Uninitialized-state checks first.
    RUN_TEST(test_setDuty_before_init_fails);
    RUN_TEST(test_setOutputEnabled_before_init_fails);
    RUN_TEST(test_getOutputEnabled_before_init_fails);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_phases);
    RUN_TEST(test_init_too_many_phases);
    RUN_TEST(test_init_rejects_out_of_range_channel);
    RUN_TEST(test_init_rejects_out_of_range_ocUnit);

    RUN_TEST(test_duty_maps_zero_half_full);
    RUN_TEST(test_duty_routes_to_each_phase_unit);
    RUN_TEST(test_duty_out_of_range_rejected_leaves_compare);
    RUN_TEST(test_duty_out_of_range_phase_rejected);

    RUN_TEST(test_output_enable_gates_bridge_and_reports);
    RUN_TEST(test_duty_while_disabled_takes_effect_at_reenable);
    RUN_TEST(test_reported_disabled_after_break);
    RUN_TEST(test_getOutputEnabled_null_rejected);

    return UNITY_END();
}
