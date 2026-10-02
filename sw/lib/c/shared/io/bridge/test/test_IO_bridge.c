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
        .timeBasePeripheral = TIMEBASE_PERIPH,
        // U and V are the sampled pair; W is derived, so it carries no complement.
        .phase = {
            [IO_BRIDGE_PHASE_U] = { .tim = HW_TIM_CHANNEL_PWM_U,
                                    .complementPhase = IO_BRIDGE_PHASE_V,
                                    .currentSense = { HW_ADC_CHANNEL_1, SENSE_U_IN, SENSE_U_INJ,
                                                      SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A } },
            [IO_BRIDGE_PHASE_V] = { .tim = HW_TIM_CHANNEL_PWM_V,
                                    .complementPhase = IO_BRIDGE_PHASE_U,
                                    .currentSense = { HW_ADC_CHANNEL_2, SENSE_V_IN, SENSE_V_INJ,
                                                      SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A } },
            [IO_BRIDGE_PHASE_W] = { .tim = HW_TIM_CHANNEL_PWM_W,
                                    .complementPhase = IO_BRIDGE_PHASE_COUNT,
                                    .currentSense = { HW_ADC_CHANNEL_1, SENSE_W_IN, IO_BRIDGE_INJECTED_NONE,
                                                      SENSE_PHASE_BIAS_V, SENSE_PHASE_V_PER_A } },
        },
        .busCurrent = { HW_ADC_CHANNEL_2, SENSE_BUS_IN, IO_BRIDGE_INJECTED_NONE,
                        0.0f, SENSE_BUS_V_PER_A },
        .injectedPairWindow_us = TEST_PAIR_WINDOW_US };

    config = (IO_bridge_config_S){
        .channels           = bridgeCfg,
        .numChannels        = 1U };   // the motor bridge; the second slot is for the multi-bridge checks
}

// What the per-cycle callback saw when it last ran. One call per completed
// triple, so cycleCalls is how the suite observes a triple forming at all. The
// driver keeps its registration in static storage, so every cycle test
// re-registers its own.
static uint32_t            cycleCalls;
static void *              cycleContext;
static IO_bridge_channel_E cycleChannel;
static float32_t           cycleW_amps;
static bool                cycleW_valid;
static uint32_t            cycleToken;

static void onCycle(IO_bridge_channel_E channel, void * context)
{
    cycleCalls++;
    cycleChannel = channel;
    cycleContext = context;
    cycleW_valid = IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_W, &cycleW_amps);
}

void setUp(void)
{
    mock_HW_TIM_reset(TEST_PERIOD);
    mock_HW_ADC_reset();
    buildGoodConfig();

    cycleCalls   = 0U;
    cycleContext = NULL;
    cycleChannel = IO_BRIDGE_CHANNEL_COUNT;
    cycleW_amps  = 0.0f;
    cycleW_valid = false;
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

// [test->fw~io_bridge_007~1]
static void test_registerCycleCallback_before_init_fails(void)
{
    TEST_ASSERT_FALSE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));
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
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_V].tim = HW_TIM_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1]
static void test_init_rejects_phases_on_different_peripherals(void)
{
    // Phase W points at a channel owned by a different peripheral.
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_W].tim = HW_TIM_CHANNEL_OTHER;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1] a phase cannot be its own complement: the pair
// would complete against its own stamp.
static void test_init_rejects_self_complement(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_U].complementPhase = IO_BRIDGE_PHASE_U;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1] U names V, V names W: the table is asymmetric, so
// no pair agrees on its partner.
static void test_init_rejects_asymmetric_complement_pair(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_V].complementPhase = IO_BRIDGE_PHASE_W;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1] the derived phase carries IO_BRIDGE_PHASE_COUNT and
// is accepted; naming a phase already paired elsewhere is not.
static void test_init_accepts_derived_phase_without_complement(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_W].complementPhase = IO_BRIDGE_PHASE_U;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));

    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_W].complementPhase = IO_BRIDGE_PHASE_COUNT;
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1] an injected ADC channel delivers to one bridge:
// a second bridge claiming the same channel is rejected
static void test_init_rejects_two_bridges_sharing_an_injected_adc_channel(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_SECOND] = bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR];
    config.numChannels = 2U;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// [test->fw~io_bridge_001~1] a second bridge with no injected senses claims
// no ADC channel, so both bridges initialize and the first keeps its callbacks
static void test_init_accepts_a_second_bridge_without_injected_senses(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_SECOND] = bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR];
    for (uint8_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        bridgeCfg[IO_BRIDGE_CHANNEL_SECOND].phase[phase].currentSense.injectedIndex = IO_BRIDGE_INJECTED_NONE;
    }
    config.numChannels = 2U;
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_EQUAL_UINT32(1U, mock_HW_ADC_getRegistrationCount(HW_ADC_CHANNEL_1));
    TEST_ASSERT_EQUAL_UINT32(1U, mock_HW_ADC_getRegistrationCount(HW_ADC_CHANNEL_2));
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

/* ---- fw~io_bridge_005: sense readout ---- */

// Volts that decode to a given phase current through the bipolar front end.
static float32_t ampsToVolts(float32_t amps)
{
    return SENSE_PHASE_BIAS_V + (amps * SENSE_PHASE_V_PER_A);
}

// Read one phase's reported current, asserting the read itself succeeded.
static float32_t phaseAmps(IO_bridge_phase_E phase)
{
    float32_t amps = 0.0f;
    TEST_ASSERT_TRUE(IO_bridge_getPhaseCurrent(MOTOR, phase, &amps));
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

// Publish one crest sample on a phase's own ADC.
static void fireInjected(HW_ADC_channels_E adc, uint8_t slot, float32_t amps)
{
    mock_HW_ADC_setInjectedVolts(adc, slot, ampsToVolts(amps));
    mock_HW_ADC_fireInjected(adc, HW_ADC_CONVERSION_STATUS_OK);
}

// One complete crest pair at a fresh trigger: U then V, so the second completion
// forms the triple.
static void fireInjectedPair(float32_t ampsU, float32_t ampsV)
{
    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsU);
    fireInjected(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsV);
}

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

// Must run before any completion is fired: the driver holds its samples in
// static storage that no test resets.
// [test->fw~io_bridge_005~1]
static void test_getPhaseCurrent_zero_before_first_sample(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 42.0f;
    TEST_ASSERT_TRUE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, &amps));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, amps);
    TEST_ASSERT_TRUE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_W, &amps));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, amps);
}

// Bias + gain applied, sign preserved across the zero-current midpoint.
// [test->fw~io_bridge_005~1]
static void test_getPhaseCurrent_scales_and_signs(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, SENSE_PHASE_BIAS_V);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, phaseAmps(IO_BRIDGE_PHASE_U));

    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_U));

    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, -2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -2.0f, phaseAmps(IO_BRIDGE_PHASE_U));
}

// The phase reader is the injected path only: a regular-sequence sample on the
// same pin never reaches it.
// [test->fw~io_bridge_005~1]
static void test_getPhaseCurrent_follows_injected_not_regular(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_U));

    // A fresh regular conversion of U's pin leaves the reported current alone.
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_1, SENSE_U_IN, ampsToVolts(-5.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_U));

    // The next crest sample does move it.
    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, phaseAmps(IO_BRIDGE_PHASE_U));
}

// An unconfigured phase sense (voltsPerAmp == 0) publishes nothing rather than
// dividing by zero: the phase holds its last value and no triple forms.
// [test->fw~io_bridge_005~1]
static void test_getPhaseCurrent_unconfigured_sense_publishes_nothing(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_U].currentSense.voltsPerAmp = 0.0f;
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    nextTrigger();
    const float32_t held = phaseAmps(IO_BRIDGE_PHASE_U);

    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 3.0f);
    fireInjected(HW_ADC_CHANNEL_2, SENSE_V_INJ, 1.0f);

    TEST_ASSERT_EQUAL_FLOAT(held, phaseAmps(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(0U, cycleCalls);   // V alone cannot complete a triple
}

// The bus reader follows the regular sequence, sample to sample.
// [test->fw~io_bridge_005~1]
static void test_getBusCurrent_scales_and_follows_regular(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 0.0f;
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_2, SENSE_BUS_IN, 0.9f);   // 0.9 / 0.6 = 1.5 A
    TEST_ASSERT_TRUE(IO_bridge_getBusCurrent(MOTOR, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5f, amps);

    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_2, SENSE_BUS_IN, 0.3f);   // 0.3 / 0.6 = 0.5 A
    TEST_ASSERT_TRUE(IO_bridge_getBusCurrent(MOTOR, &amps));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, amps);
}

// An unconfigured bus sense (voltsPerAmp == 0) fails rather than dividing by zero.
// [test->fw~io_bridge_005~1]
static void test_getBusCurrent_unconfigured_sense_fails(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].busCurrent.voltsPerAmp = 0.0f;
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 7.0f;
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_2, SENSE_BUS_IN, 0.9f);
    TEST_ASSERT_FALSE(IO_bridge_getBusCurrent(MOTOR, &amps));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, amps);
}

// A failed ADC read (input never set) propagates as false, leaving the
// destination unchanged — never a false 0 A to the overcurrent monitor.
// [test->fw~io_bridge_005~1]
static void test_getBusCurrent_read_failure_propagates(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    float32_t amps = 55.0f;
    TEST_ASSERT_FALSE(IO_bridge_getBusCurrent(MOTOR, &amps));
    TEST_ASSERT_EQUAL_FLOAT(55.0f, amps);
}

static void test_getPhaseCurrent_out_of_range_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    float32_t amps = 0.0f;
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_COUNT, &amps));
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(IO_BRIDGE_CHANNEL_COUNT, IO_BRIDGE_PHASE_U, &amps));
}

static void test_getCurrent_null_rejected(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_FALSE(IO_bridge_getPhaseCurrent(MOTOR, IO_BRIDGE_PHASE_U, NULL));
    TEST_ASSERT_FALSE(IO_bridge_getBusCurrent(MOTOR, NULL));
}

/* ---- fw~io_bridge_006: crest (injected) current sampling ---- */

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
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR].phase[IO_BRIDGE_PHASE_U].currentSense.injectedIndex =
        HW_ADC_INJECTED_INPUTS_PER_CHANNEL;
    TEST_ASSERT_FALSE(IO_bridge_init(&config));
}

// Each phase is published by the completion of its own ADC, decoded through its
// own bias and gain; the other phase holds the value its ADC last gave it.
// [test->fw~io_bridge_005~1]
// [test->fw~io_bridge_006~1]
static void test_injected_each_phase_publishes_on_its_own_completion(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(1.0f, 1.0f);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    // ADC1 alone: U moves to its new sample, V keeps the one ADC2 gave it, and
    // no triple forms (V's stamp belongs to the previous trigger).
    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(-4.0f));
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, phaseAmps(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    // ADC2's completion publishes V and completes this trigger's triple.
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, phaseAmps(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_EQUAL_UINT32(2U, cycleCalls);
}

// A faulted conversion publishes nothing: the phase holds its last good value
// and the trigger forms no triple.
static void test_injected_failed_status_holds_last_good(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(2.0f, 2.0f);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, 3.0f);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_FAULT);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
}

// W is completed by the second callback of a pair, never the first.
// [test->fw~io_bridge_006~1]
static void test_injected_w_completes_only_on_second_callback(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    nextTrigger();
    const float32_t heldW = phaseAmps(IO_BRIDGE_PHASE_W);

    // First of the pair: its partner has no sample at this trigger yet.
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 3.0f);
    TEST_ASSERT_EQUAL_UINT32(0U, cycleCalls);
    TEST_ASSERT_EQUAL_FLOAT(heldW, phaseAmps(IO_BRIDGE_PHASE_W));

    // Second completes the pair, exactly once: W = -(3 - 1).
    fireInjected(HW_ADC_CHANNEL_2, SENSE_V_INJ, -1.0f);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -2.0f, phaseAmps(IO_BRIDGE_PHASE_W));

    // Once the trigger moves on, a lone U cannot re-pair with V's stale sample.
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
}

// N triggers form N triples, each from that trigger's own pair of samples.
// [test->fw~io_bridge_006~1]
static void test_injected_every_trigger_forms_one_triple(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    // Each trigger's pair sums differently, so W proves the triple came from
    // that trigger's samples and not a carried-over one.
    static const float32_t ampsU[4] = { 1.0f,  3.0f, -2.0f, 0.5f };
    static const float32_t ampsV[4] = { 1.0f, -1.0f, -1.0f, 0.25f };

    for (uint32_t cycle = 0U; cycle < 4U; cycle++)
    {
        fireInjectedPair(ampsU[cycle], ampsV[cycle]);
        TEST_ASSERT_EQUAL_UINT32(cycle + 1U, cycleCalls);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, -(ampsU[cycle] + ampsV[cycle]), phaseAmps(IO_BRIDGE_PHASE_W));
    }
}

// A dropped partner leaves W without a same-trigger pair, so it holds its last
// value — and the very next trigger pairs cleanly, with nothing to resync.
static void test_injected_dropped_partner_holds_w_then_next_trigger_pairs(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(2.0f, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, phaseAmps(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    // V's conversion is dropped: U alone cannot complete W.
    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 5.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, phaseAmps(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    // The next trigger pairs immediately — no resync, no lost period beyond
    // the one that dropped.
    nextTrigger();
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(1.0f));
    fireInjected(HW_ADC_CHANNEL_2, SENSE_V_INJ, -3.0f);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_EQUAL_UINT32(2U, cycleCalls);
}

// Samples stamped a whole trigger period apart are two different triggers, so
// they never pair however their arrival order interleaves.
// [test->fw~io_bridge_006~1]
static void test_injected_different_triggers_never_pair(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(1.0f, 1.0f);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    // U from one trigger, V from the next: both phases update, W does not.
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
}

// With no time base every stamp would read zero and every sample would look
// simultaneous, so the driver publishes nothing rather than pair blindly.
static void test_injected_without_time_base_publishes_nothing(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(2.0f, 2.0f);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);

    nextTrigger();
    mock_HW_TIM_setGetCounterFails(true);
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 7.0f);
    fireInjected(HW_ADC_CHANNEL_2, SENSE_V_INJ, 7.0f);
    mock_HW_TIM_setGetCounterFails(false);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, phaseAmps(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
}

// An unreadable injected slot publishes nothing, so a stale sample is never
// overwritten by a reading the ADC did not take.
static void test_injected_unreadable_slot_publishes_nothing(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    nextTrigger();
    const float32_t heldU = phaseAmps(IO_BRIDGE_PHASE_U);

    // Nothing has been written to either slot since mock_HW_ADC_reset.
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);

    TEST_ASSERT_EQUAL_FLOAT(heldU, phaseAmps(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(0U, cycleCalls);
}

/* ---- fw~io_bridge_007: per-cycle callback ---- */

// [test->fw~io_bridge_007~1]
static void test_registerCycleCallback_rejects_bad_arguments(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));

    TEST_ASSERT_FALSE(IO_bridge_registerCycleCallback(IO_BRIDGE_CHANNEL_COUNT, onCycle, &cycleToken));
    TEST_ASSERT_FALSE(IO_bridge_registerCycleCallback(MOTOR, NULL, &cycleToken));
}

// One call per completed pair, carrying the registered context — never on the
// first of the pair, never twice for the same pair.
// [test->fw~io_bridge_007~1]
static void test_cycle_callback_fires_once_per_pair(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    nextTrigger();
    fireInjected(HW_ADC_CHANNEL_1, SENSE_U_INJ, 2.0f);
    TEST_ASSERT_EQUAL_UINT32(0U, cycleCalls);

    fireInjected(HW_ADC_CHANNEL_2, SENSE_V_INJ, 2.0f);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
    TEST_ASSERT_EQUAL_PTR(&cycleToken, cycleContext);
    TEST_ASSERT_EQUAL_INT(MOTOR, cycleChannel);

    // The next trigger's pair calls it again, once.
    fireInjectedPair(2.0f, 2.0f);
    TEST_ASSERT_EQUAL_UINT32(2U, cycleCalls);
}

// The triple is published before the call, so the callback's own read of W
// already sees this pair's KCL value.
// [test->fw~io_bridge_007~1]
static void test_cycle_callback_reads_derived_w(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(3.0f, -1.0f);

    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
    TEST_ASSERT_TRUE(cycleW_valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -2.0f, cycleW_amps);
}

// A later registration replaces the earlier: the second context is the one the
// callback is handed.
// [test->fw~io_bridge_007~1]
static void test_cycle_callback_reregistration_replaces(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, NULL));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    fireInjectedPair(1.0f, 1.0f);

    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
    TEST_ASSERT_EQUAL_PTR(&cycleToken, cycleContext);
}

// Samples farther apart than the pair window start a new pair rather than
// completing one, so no triple and no call.
// [test->fw~io_bridge_006~1]
// [test->fw~io_bridge_007~1]
static void test_cycle_callback_silent_outside_pair_window(void)
{
    TEST_ASSERT_TRUE(IO_bridge_init(&config));
    TEST_ASSERT_TRUE(IO_bridge_registerCycleCallback(MOTOR, onCycle, &cycleToken));

    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_1, SENSE_U_INJ, ampsToVolts(1.0f));
    mock_HW_ADC_setInjectedVolts(HW_ADC_CHANNEL_2, SENSE_V_INJ, ampsToVolts(1.0f));

    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    nextTrigger();
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_2, HW_ADC_CONVERSION_STATUS_OK);

    TEST_ASSERT_EQUAL_UINT32(0U, cycleCalls);

    // That lone V is the new pair's first sample: U at the same trigger pairs
    // with it immediately.
    mock_HW_ADC_fireInjected(HW_ADC_CHANNEL_1, HW_ADC_CONVERSION_STATUS_OK);
    TEST_ASSERT_EQUAL_UINT32(1U, cycleCalls);
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
    RUN_TEST(test_registerCycleCallback_before_init_fails);
    RUN_TEST(test_getPhaseCurrent_before_init_fails);
    RUN_TEST(test_getBusCurrent_before_init_fails);

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_out_of_range_phase_channel);
    RUN_TEST(test_init_rejects_phases_on_different_peripherals);
    RUN_TEST(test_init_rejects_self_complement);
    RUN_TEST(test_init_rejects_asymmetric_complement_pair);
    RUN_TEST(test_init_accepts_derived_phase_without_complement);
    RUN_TEST(test_init_rejects_two_bridges_sharing_an_injected_adc_channel);
    RUN_TEST(test_init_accepts_a_second_bridge_without_injected_senses);

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

    // The zero-before-first-sample check and the injected registration checks
    // must precede the first fired completion, which leaves samples in
    // unresettable static storage.
    RUN_TEST(test_getPhaseCurrent_zero_before_first_sample);
    RUN_TEST(test_init_registers_injected_callback_once_per_adc);
    RUN_TEST(test_init_fails_when_injected_registration_fails);
    RUN_TEST(test_init_rejects_out_of_range_injected_index);
    RUN_TEST(test_injected_unreadable_slot_publishes_nothing);

    RUN_TEST(test_getPhaseCurrent_scales_and_signs);
    RUN_TEST(test_getPhaseCurrent_follows_injected_not_regular);
    RUN_TEST(test_getPhaseCurrent_unconfigured_sense_publishes_nothing);
    RUN_TEST(test_getBusCurrent_scales_and_follows_regular);
    RUN_TEST(test_getBusCurrent_unconfigured_sense_fails);
    RUN_TEST(test_getBusCurrent_read_failure_propagates);
    RUN_TEST(test_getPhaseCurrent_out_of_range_rejected);
    RUN_TEST(test_getCurrent_null_rejected);

    RUN_TEST(test_injected_each_phase_publishes_on_its_own_completion);
    RUN_TEST(test_injected_failed_status_holds_last_good);
    RUN_TEST(test_injected_w_completes_only_on_second_callback);
    RUN_TEST(test_injected_every_trigger_forms_one_triple);
    RUN_TEST(test_injected_dropped_partner_holds_w_then_next_trigger_pairs);
    RUN_TEST(test_injected_different_triggers_never_pair);
    RUN_TEST(test_injected_without_time_base_publishes_nothing);

    RUN_TEST(test_registerCycleCallback_rejects_bad_arguments);
    RUN_TEST(test_cycle_callback_fires_once_per_pair);
    RUN_TEST(test_cycle_callback_reads_derived_w);
    RUN_TEST(test_cycle_callback_reregistration_replaces);
    RUN_TEST(test_cycle_callback_silent_outside_pair_window);

    return UNITY_END();
}
