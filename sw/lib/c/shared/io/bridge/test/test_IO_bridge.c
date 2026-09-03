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
// The free-running base the driver stamps injected samples with, and the two
// spans that decide pairing: a window samples must land inside, and the trigger
// period that separates one PWM crest from the next.
#define TIMEBASE_PERIPH        (HW_TIM_PERIPHERAL_2)
#define TEST_PAIR_WINDOW_US    (25U)
#define TEST_TRIGGER_PERIOD_US (50U)
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

// U and V each occupy slot 0 of their own ADC's injected sequence, as on the
// board; W and the bus shunt have no crest sample.
#define SENSE_U_INJ          (0U)
#define SENSE_V_INJ          (0U)

static IO_bridge_channelConfig_S bridgeCfg[IO_BRIDGE_CHANNEL_COUNT];
static IO_bridge_config_S        config;

static void buildGoodConfig(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR] = (IO_bridge_channelConfig_S){
        .phaseU = HW_TIM_CHANNEL_PWM_U,
        .phaseV = HW_TIM_CHANNEL_PWM_V,
        .phaseW = HW_TIM_CHANNEL_PWM_W,
        .phaseCurrent = {
            [IO_BRIDGE_PHASE_U] = { HW_ADC_CHANNEL_1, SENSE_U_IN, SENSE_U_INJ,
                                    SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A },
            [IO_BRIDGE_PHASE_V] = { HW_ADC_CHANNEL_2, SENSE_V_IN, SENSE_V_INJ,
                                    SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A },
            [IO_BRIDGE_PHASE_W] = { HW_ADC_CHANNEL_1, SENSE_W_IN, IO_BRIDGE_INJECTED_NONE,
                                    SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A },
        },
        .busCurrent = { HW_ADC_CHANNEL_2, SENSE_BUS_IN, IO_BRIDGE_INJECTED_NONE,
                        0.0f, SENSE_BUS_V_PER_A },
        .injectedPairWindow_us = TEST_PAIR_WINDOW_US };

    config = (IO_bridge_config_S){
        .channels           = bridgeCfg,
        .numChannels        = IO_BRIDGE_CHANNEL_COUNT,
        .timeBasePeripheral = TIMEBASE_PERIPH };
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

static void test_getInjectedPhaseCurrent_before_init_fails(void)
{
    float32_t amps = 123.0f;
    TEST_ASSERT_FALSE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_EQUAL_FLOAT(123.0f, amps);
}

static void test_getInjectedUpdateCount_before_init_fails(void)
{
    uint32_t count = 99U;
    TEST_ASSERT_FALSE(IO_bridge_getInjectedUpdateCount(MOTOR, IO_BRIDGE_PHASE_U, &count));
    TEST_ASSERT_EQUAL_UINT32(99U, count);
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

/* ---- crest (injected) current sampling ---- */

// Volts that decode to a given phase current through the bipolar front end.
static float32_t ampsToVolts(float32_t amps)
{
    return SENSE_PHASE_BIAS_V + (amps * SENSE_PHASE_V_PER_A);
}

// Read one phase's update counter, asserting the accessor itself succeeded.
static uint32_t injectedCount(IO_bridge_phase_E phase)
{
    uint32_t count = 0U;
    TEST_ASSERT_TRUE(IO_bridge_getInjectedUpdateCount(MOTOR, phase, &count));
    return count;
}

// Read one phase's crest current, asserting a sample was published.
static float32_t injectedAmps(IO_bridge_phase_E phase)
{
    float32_t amps = 0.0f;
    TEST_ASSERT_TRUE(IO_bridge_getInjectedPhaseCurrent(MOTOR, phase, &amps));
    return amps;
}

// Move to the next PWM trigger. The step is a whole trigger period, so samples
// either side of it lie outside the pair window and can never pair. Monotonic
// across tests, since the driver keeps its stamps (it has no reset hook).
static void nextTrigger(void)
{
    static uint32_t now_us = 0U;
    now_us += TEST_TRIGGER_PERIOD_US;
    mock_HW_TIM_setCounter(TIMEBASE_PERIPH, now_us);
}

// One registration per ADC channel carrying an injected phase, however many
// phases share it: U on ADC1, V on ADC2, W derived so it registers nothing.
static void test_init_registers_injected_callback_once_per_adc(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_EQUAL_UINT32(1U, mock_HW_ADC_getRegistrationCount(HW_ADC_CHANNEL_1));
    TEST_ASSERT_EQUAL_UINT32(1U, mock_HW_ADC_getRegistrationCount(HW_ADC_CHANNEL_2));
}

static void test_init_fails_when_injected_registration_fails(void)
{
    mock_HW_ADC_failRegistration(HW_ADC_CHANNEL_2);
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

static void test_init_rejects_out_of_range_injected_index(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phaseCurrent[IO_BRIDGE_PHASE_U].injectedIndex =
        HW_ADC_INJECTED_INPUTS_PER_CHANNEL;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// Must run before any completion is fired: the driver holds its samples in
// static storage that no test resets.
static void test_getInjectedPhaseCurrent_no_sample_yet_fails(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 42.0f;
    TEST_ASSERT_FALSE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FALSE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_W, &amps));
    TEST_ASSERT_EQUAL_FLOAT(42.0f, amps);
}

// A completion decodes its slot's counts through the same bias + gain as the
// regular path, so the sinking leg comes back genuinely negative.
static void test_injected_completion_decodes_signed_amps(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    nextTrigger();

    float32_t amps = 0.0f;

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, 1.95f);   // +3.0 A
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_TRUE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, amps);

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, 1.25f);   // -4.0 A
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_TRUE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_V, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, amps);
}

// Each completion only touches the phases sitting on its own ADC.
static void test_injected_completion_routes_by_adc_channel(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    nextTrigger();

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, 1.85f);   // +2.0 A
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, 1.85f);   // +2.0 A
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, 1.45f);   // -2.0 A
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);

    // Only ADC2 completed since U's slot changed, so U still reads the +2.0 A
    // it was sampled with.
    float32_t amps = 0.0f;
    TEST_ASSERT_TRUE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, amps);
    TEST_ASSERT_TRUE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_V, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, amps);
}

static void test_injected_failed_status_holds_last_good(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    nextTrigger();

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(2.0f));
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    const uint32_t countAfterGood = injectedCount(IO_BRIDGE_PHASE_U);

    // A faulted conversion publishes nothing, neither value nor update.
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, 3.0f);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_FAULT);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, injectedAmps(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(countAfterGood, injectedCount(IO_BRIDGE_PHASE_U));
}

// W is completed by the second callback of a pair, never the first.
static void test_injected_w_completes_only_on_second_callback(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    nextTrigger();

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(3.0f));
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(-1.0f));

    const uint32_t countBefore = injectedCount(IO_BRIDGE_PHASE_W);

    // First of the pair: its partner has no sample at this trigger yet.
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(countBefore, injectedCount(IO_BRIDGE_PHASE_W));

    // Second completes the pair, exactly once.
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -2.0f, injectedAmps(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_EQUAL_UINT32(countBefore + 1U, injectedCount(IO_BRIDGE_PHASE_W));

    // Once the trigger moves on, a lone U cannot re-pair with V's stale sample.
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(countBefore + 1U, injectedCount(IO_BRIDGE_PHASE_W));
}

// Each phase counts its own samples, so U advances alone when V's ADC is quiet.
static void test_injected_per_phase_counts_advance_independently(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    nextTrigger();

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(1.0f));
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(1.0f));

    const uint32_t beforeU = injectedCount(IO_BRIDGE_PHASE_U);
    const uint32_t beforeV = injectedCount(IO_BRIDGE_PHASE_V);

    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(beforeU + 1U, injectedCount(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(beforeV, injectedCount(IO_BRIDGE_PHASE_V));

    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(beforeV + 1U, injectedCount(IO_BRIDGE_PHASE_V));
}

// A dropped partner leaves W without a same-trigger pair, so it holds its last
// value — and the very next trigger pairs cleanly, with nothing to resync.
static void test_injected_dropped_partner_holds_w_then_next_trigger_pairs(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(2.0f));
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(2.0f));
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, injectedAmps(IO_BRIDGE_PHASE_W));
    const uint32_t countAfterPair = injectedCount(IO_BRIDGE_PHASE_W);

    // V's conversion is dropped: U alone cannot complete W.
    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(5.0f));
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, injectedAmps(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_EQUAL_UINT32(countAfterPair, injectedCount(IO_BRIDGE_PHASE_W));

    // The next trigger pairs immediately — no resync, no lost period beyond
    // the one that dropped.
    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(1.0f));
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(-3.0f));
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, injectedAmps(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_EQUAL_UINT32(countAfterPair + 1U, injectedCount(IO_BRIDGE_PHASE_W));
}

// Samples stamped a whole trigger period apart are two different triggers, so
// they never pair however their arrival order interleaves.
static void test_injected_different_triggers_never_pair(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(1.0f));
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(1.0f));
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    const uint32_t paired = injectedCount(IO_BRIDGE_PHASE_W);

    // U from one trigger, V from the next: both phases update, W does not.
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(paired, injectedCount(IO_BRIDGE_PHASE_W));
}

// With no time base every stamp would read zero and every sample would look
// simultaneous, so the driver publishes nothing rather than pair blindly.
static void test_injected_without_time_base_publishes_nothing(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    nextTrigger();

    const uint32_t beforeU = injectedCount(IO_BRIDGE_PHASE_U);
    mock_HW_TIM_setGetCounterFails(true);
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(2.0f));
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_TIM_setGetCounterFails(false);

    TEST_ASSERT_EQUAL_UINT32(beforeU, injectedCount(IO_BRIDGE_PHASE_U));
}

// An unreadable injected slot publishes nothing, so a stale sample is never
// overwritten by a reading the ADC did not take.
static void test_injected_unreadable_slot_publishes_nothing(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    const uint32_t before = injectedCount(IO_BRIDGE_PHASE_U);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(before, injectedCount(IO_BRIDGE_PHASE_U));
}

static void test_getInjected_guards_reject_bad_arguments(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 0.0f;
    uint32_t count = 0U;
    TEST_ASSERT_FALSE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_COUNT, &amps));
    TEST_ASSERT_FALSE(IO_bridge_getInjectedPhaseCurrent(IO_BRIDGE_CHANNEL_COUNT, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_FALSE(IO_bridge_getInjectedPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, NULL));
    TEST_ASSERT_FALSE(IO_bridge_getInjectedUpdateCount(MOTOR, IO_BRIDGE_PHASE_COUNT, &count));
    TEST_ASSERT_FALSE(IO_bridge_getInjectedUpdateCount(IO_BRIDGE_CHANNEL_COUNT, IO_BRIDGE_PHASE_U, &count));
    TEST_ASSERT_FALSE(IO_bridge_getInjectedUpdateCount(MOTOR, IO_BRIDGE_PHASE_U, NULL));
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
    RUN_TEST(test_getInjectedPhaseCurrent_before_init_fails);
    RUN_TEST(test_getInjectedUpdateCount_before_init_fails);

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

    // Injected registration + the no-sample-yet check must precede the first
    // fired completion, which leaves samples in unresettable static storage.
    RUN_TEST(test_init_registers_injected_callback_once_per_adc);
    RUN_TEST(test_init_fails_when_injected_registration_fails);
    RUN_TEST(test_init_rejects_out_of_range_injected_index);
    RUN_TEST(test_getInjectedPhaseCurrent_no_sample_yet_fails);
    RUN_TEST(test_injected_without_time_base_publishes_nothing);
    RUN_TEST(test_injected_unreadable_slot_publishes_nothing);

    RUN_TEST(test_injected_completion_decodes_signed_amps);
    RUN_TEST(test_injected_completion_routes_by_adc_channel);
    RUN_TEST(test_injected_failed_status_holds_last_good);
    RUN_TEST(test_injected_w_completes_only_on_second_callback);
    RUN_TEST(test_injected_per_phase_counts_advance_independently);
    RUN_TEST(test_injected_dropped_partner_holds_w_then_next_trigger_pairs);
    RUN_TEST(test_injected_different_triggers_never_pair);
    RUN_TEST(test_getInjected_guards_reject_bad_arguments);

    return UNITY_END();
}
