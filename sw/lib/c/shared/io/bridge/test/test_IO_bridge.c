#include "IO_bridge.h"
#include "mock_HW_TIM.h"
#include "mock_HW_ADC.h"
#include "unity.h"

// The motor bridge's three phases share HW_TIM_PERIPHERAL_1 via channels
// PWM_U/V/W, and that peripheral's master output enable gates the whole bridge.
// A round period keeps the duty math exact (half = 2000, full = 4000) without
// depending on the real ARR.
#define TEST_PERIOD    (4000U)
#define BRIDGE_PERIPH  (HW_TIM_PERIPHERAL_1)
#define MOTOR          (IO_BRIDGE_CHANNEL_MOTOR)

// Current-sense front ends the current tests read back through. Phase shunts
// are biased (bipolar) INA240-style: i = (v - 1.65) / 0.1; the bus shunt is
// ground-referenced: i = v / 0.6. IN#s span both ADCs to prove routing.
#define SENSE_PHASE_BIAS_V   (1.65f)
#define SENSE_PHASE_V_PER_A  (0.1f)
#define SENSE_BUS_V_PER_A    (0.6f)
#define SENSE_U_IN           (6U)
#define SENSE_V_IN           (7U)
#define SENSE_W_IN           (8U)
#define SENSE_BUS_IN         (11U)

static IO_bridge_channelConfig_S bridgeCfg[IO_BRIDGE_CHANNEL_COUNT];
static IO_bridge_config_S        config;

static void buildGoodConfig(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR] = (IO_bridge_channelConfig_S){
        .phaseU = HW_TIM_CHANNEL_PWM_U,
        .phaseV = HW_TIM_CHANNEL_PWM_V,
        .phaseW = HW_TIM_CHANNEL_PWM_W,
        .phaseCurrent = {
            [IO_BRIDGE_PHASE_U] = { HW_ADC_CHANNEL_1, SENSE_U_IN, SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A },
            [IO_BRIDGE_PHASE_V] = { HW_ADC_CHANNEL_2, SENSE_V_IN, SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A },
            [IO_BRIDGE_PHASE_W] = { HW_ADC_CHANNEL_1, SENSE_W_IN, SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A },
        },
        .busCurrent = { HW_ADC_CHANNEL_2, SENSE_BUS_IN, 0.0f, SENSE_BUS_V_PER_A } };

    config = (IO_bridge_config_S){
        .channels = bridgeCfg, .numChannels = IO_BRIDGE_CHANNEL_COUNT };
}

void setUp(void)
{
    mock_HW_TIM_reset(TEST_PERIOD);
    mock_HW_ADC_reset();
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- uninitialized-state checks (must precede any successful init, since the
        driver keeps static config and has no reset hook) ---- */

// [test->fw~io_bridge_002~1]
static void test_setPhaseDuty_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));
}

// [test->fw~io_bridge_003~1]
static void test_setOutputEnabled_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_bridge_setOutputEnabled(MOTOR, true));
    TEST_ASSERT_FALSE(mock_HW_TIM_getMoe(BRIDGE_PERIPH));
}

// [test->fw~io_bridge_003~1]
static void test_getOutputEnabled_before_init_fails(void)
{
    bool enabled = true;
    TEST_ASSERT_FALSE(IO_bridge_getOutputEnabled(MOTOR, &enabled));
}

// [test->fw~io_bridge_004~1]
static void test_setPhaseOutputEnabled_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_bridge_setPhaseOutputEnabled(MOTOR, IO_BRIDGE_PHASE_U, true));
    TEST_ASSERT_FALSE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_U));
}

static void test_clearBreakFlags_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_bridge_clearBreakFlags(MOTOR));
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getBreakFlagsClearCount(BRIDGE_PERIPH));
}

/* ---- fw~io_bridge_001: init + config validation ---- */

// [test->fw~io_bridge_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    // Every phase's output-compare unit is enabled up front...
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_U));
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_V));
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_W));
    // ...but the bridge stays dark: MOE off until IO_bridge_setOutputEnabled.
    TEST_ASSERT_FALSE(mock_HW_TIM_getMoe(BRIDGE_PERIPH));
}

// [test->fw~io_bridge_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(IO_bridge_init(NULL));
}

// [test->fw~io_bridge_001~1]
static void test_init_null_channels(void)
{
    config.channels = NULL;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1]
static void test_init_too_many_channels(void)
{
    config.numChannels = IO_BRIDGE_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1]
static void test_init_rejects_out_of_range_phase_channel(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phaseV = HW_TIM_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1]
static void test_init_rejects_phases_on_different_peripherals(void)
{
    // Phase W points at a channel owned by a different peripheral.
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phaseW = HW_TIM_CHANNEL_OTHER;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

/* ---- fw~io_bridge_002: per-phase duty command ---- */

// [test->fw~io_bridge_002~1]
static void test_duty_maps_zero_half_full(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 0.0f));
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));

    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 2U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));

    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 1.0f));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));
}

// [test->fw~io_bridge_002~1]
static void test_duty_routes_to_each_phase(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_V, 0.25f));
    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_W, 0.75f));

    // V -> PWM_V, W -> PWM_W; U untouched.
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 4U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_V));
    TEST_ASSERT_EQUAL_UINT32((TEST_PERIOD * 3U) / 4U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_W));
}

// [test->fw~io_bridge_002~1]
static void test_duty_out_of_range_rejected_leaves_compare(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 0.5f));
    const uint32_t before = mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U);

    TEST_ASSERT_FALSE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 1.5f));
    TEST_ASSERT_FALSE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, -0.1f));
    TEST_ASSERT_EQUAL_UINT32(before, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));
}

// [test->fw~io_bridge_002~1]
static void test_duty_out_of_range_phase_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_FALSE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_COUNT, 0.5f));
}

/* ---- fw~io_bridge_003: bridge output enable ---- */

// [test->fw~io_bridge_003~1]
static void test_output_enable_gates_bridge_and_reports(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    bool enabled = true;
    TEST_ASSERT_TRUE(IO_bridge_getOutputEnabled(MOTOR, &enabled));
    TEST_ASSERT_FALSE(enabled);                       // starts disabled

    TEST_ASSERT_TRUE(IO_bridge_setOutputEnabled(MOTOR, true));
    TEST_ASSERT_TRUE(mock_HW_TIM_getMoe(BRIDGE_PERIPH));
    TEST_ASSERT_TRUE(IO_bridge_getOutputEnabled(MOTOR, &enabled));
    TEST_ASSERT_TRUE(enabled);                        // reported matches commanded

    TEST_ASSERT_TRUE(IO_bridge_setOutputEnabled(MOTOR, false));
    TEST_ASSERT_FALSE(mock_HW_TIM_getMoe(BRIDGE_PERIPH));
    TEST_ASSERT_TRUE(IO_bridge_getOutputEnabled(MOTOR, &enabled));
    TEST_ASSERT_FALSE(enabled);
}

// [test->fw~io_bridge_003~1]
static void test_duty_while_disabled_takes_effect_at_reenable(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_setOutputEnabled(MOTOR, false));

    // A duty command while disabled is accepted and lands on the compare
    // register, even though no output is driven yet.
    TEST_ASSERT_TRUE(IO_bridge_setPhaseDuty(MOTOR, IO_BRIDGE_PHASE_U, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 2U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));

    // Re-enabling drives that already-staged compare value.
    TEST_ASSERT_TRUE(IO_bridge_setOutputEnabled(MOTOR, true));
    TEST_ASSERT_TRUE(mock_HW_TIM_getMoe(BRIDGE_PERIPH));
    TEST_ASSERT_EQUAL_UINT32(TEST_PERIOD / 2U, mock_HW_TIM_getCompare(HW_TIM_CHANNEL_PWM_U));
}

// [test->fw~io_bridge_003~1]
static void test_reported_disabled_after_break(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_setOutputEnabled(MOTOR, true));

    bool enabled = false;
    TEST_ASSERT_TRUE(IO_bridge_getOutputEnabled(MOTOR, &enabled));
    TEST_ASSERT_TRUE(enabled);

    // A break clears the peripheral's MOE behind the driver's back.
    mock_HW_TIM_assertBreak(BRIDGE_PERIPH);
    TEST_ASSERT_TRUE(IO_bridge_getOutputEnabled(MOTOR, &enabled));
    TEST_ASSERT_FALSE(enabled);
}

// [test->fw~io_bridge_003~1]
static void test_getOutputEnabled_null_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_FALSE(IO_bridge_getOutputEnabled(MOTOR, NULL));
}

/* ---- fw~io_bridge_004: per-phase output enable ---- */

// [test->fw~io_bridge_004~1]
static void test_phase_output_disable_holds_one_phase_only(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    // Init enabled all three; disable phase V only.
    TEST_ASSERT_TRUE(IO_bridge_setPhaseOutputEnabled(MOTOR, IO_BRIDGE_PHASE_V, false));

    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_U));
    TEST_ASSERT_FALSE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_V));
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_W));

    // Re-enabling phase V restores its output.
    TEST_ASSERT_TRUE(IO_bridge_setPhaseOutputEnabled(MOTOR, IO_BRIDGE_PHASE_V, true));
    TEST_ASSERT_TRUE(mock_HW_TIM_getOutputEnabled(HW_TIM_CHANNEL_PWM_V));
}

// [test->fw~io_bridge_004~1]
static void test_phase_output_enable_out_of_range_phase_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_FALSE(IO_bridge_setPhaseOutputEnabled(MOTOR, IO_BRIDGE_PHASE_COUNT, false));
}

/* ---- break-flag clearing ---- */

static void test_clearBreakFlags_routes_to_bridge_peripheral(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    TEST_ASSERT_TRUE(IO_bridge_clearBreakFlags(MOTOR));
    TEST_ASSERT_EQUAL_UINT32(1U, mock_HW_TIM_getBreakFlagsClearCount(BRIDGE_PERIPH));
    TEST_ASSERT_EQUAL_UINT32(0U, mock_HW_TIM_getBreakFlagsClearCount(HW_TIM_PERIPHERAL_2));
}

static void test_clearBreakFlags_out_of_range_channel_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_FALSE(IO_bridge_clearBreakFlags(IO_BRIDGE_CHANNEL_COUNT));
}

/* ---- current-sense readback ---- */

static void test_getPhaseCurrent_before_init_fails(void)
{
    float32_t amps = 123.0f;
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_EQUAL_FLOAT(123.0f, amps);   // destination untouched on failure
}

static void test_getBusCurrent_before_init_fails(void)
{
    float32_t amps = 123.0f;
    TEST_ASSERT_FALSE(IO_bridge_getBusCurrent(MOTOR, &amps));
    TEST_ASSERT_EQUAL_FLOAT(123.0f, amps);
}

// Bias + gain applied, sign preserved across the zero-current midpoint.
static void test_getPhaseCurrent_scales_and_signs(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 0.0f;

    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_1, SENSE_U_IN, 1.85f);   // +2.0 A
    TEST_ASSERT_TRUE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, amps);

    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_1, SENSE_U_IN, 1.45f);   // -2.0 A
    TEST_ASSERT_TRUE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -2.0f, amps);
}

// Each phase reads its own configured (ADC channel, IN#): setting only V's cell
// leaves U and W failing.
static void test_getPhaseCurrent_routes_to_configured_input(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 0.0f;
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_2, SENSE_V_IN, 1.75f);   // +1.0 A on V only

    TEST_ASSERT_TRUE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_V, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, amps);

    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_W, &amps));
}

static void test_getBusCurrent_scales(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 0.0f;
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_2, SENSE_BUS_IN, 0.9f);   // 0.9 / 0.6 = 1.5 A
    TEST_ASSERT_TRUE(IO_bridge_getBusCurrent(MOTOR, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5f, amps);
}

// A failed ADC read (input never set) propagates as false, leaving the
// destination unchanged — never a false 0 A to the overcurrent monitor.
static void test_getPhaseCurrent_read_failure_propagates(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 55.0f;
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_EQUAL_FLOAT(55.0f, amps);
}

// An unconfigured sense (voltsPerAmp == 0) fails rather than dividing by zero.
static void test_getPhaseCurrent_unconfigured_sense_fails(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phaseCurrent[IO_BRIDGE_PHASE_U].voltsPerAmp = 0.0f;
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 7.0f;
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_1, SENSE_U_IN, 1.85f);
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, amps);
}

static void test_getPhaseCurrent_out_of_range_phase_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    float32_t amps = 0.0f;
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_1, SENSE_U_IN, 1.85f);
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_COUNT, &amps));
}

static void test_getCurrent_null_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, NULL));
    TEST_ASSERT_FALSE(IO_bridge_getBusCurrent(MOTOR, NULL));
}

int main(void)
{
    UNITY_BEGIN();

    // Uninitialized-state checks first.
    RUN_TEST(test_setPhaseDuty_before_init_fails);
    RUN_TEST(test_setOutputEnabled_before_init_fails);
    RUN_TEST(test_getOutputEnabled_before_init_fails);
    RUN_TEST(test_setPhaseOutputEnabled_before_init_fails);
    RUN_TEST(test_clearBreakFlags_before_init_fails);
    RUN_TEST(test_getPhaseCurrent_before_init_fails);
    RUN_TEST(test_getBusCurrent_before_init_fails);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_out_of_range_phase_channel);
    RUN_TEST(test_init_rejects_phases_on_different_peripherals);

    RUN_TEST(test_duty_maps_zero_half_full);
    RUN_TEST(test_duty_routes_to_each_phase);
    RUN_TEST(test_duty_out_of_range_rejected_leaves_compare);
    RUN_TEST(test_duty_out_of_range_phase_rejected);

    RUN_TEST(test_output_enable_gates_bridge_and_reports);
    RUN_TEST(test_duty_while_disabled_takes_effect_at_reenable);
    RUN_TEST(test_reported_disabled_after_break);
    RUN_TEST(test_getOutputEnabled_null_rejected);

    RUN_TEST(test_phase_output_disable_holds_one_phase_only);
    RUN_TEST(test_phase_output_enable_out_of_range_phase_rejected);

    RUN_TEST(test_clearBreakFlags_routes_to_bridge_peripheral);
    RUN_TEST(test_clearBreakFlags_out_of_range_channel_rejected);

    RUN_TEST(test_getPhaseCurrent_scales_and_signs);
    RUN_TEST(test_getPhaseCurrent_routes_to_configured_input);
    RUN_TEST(test_getBusCurrent_scales);
    RUN_TEST(test_getPhaseCurrent_read_failure_propagates);
    RUN_TEST(test_getPhaseCurrent_unconfigured_sense_fails);
    RUN_TEST(test_getPhaseCurrent_out_of_range_phase_rejected);
    RUN_TEST(test_getCurrent_null_rejected);

    return UNITY_END();
}
