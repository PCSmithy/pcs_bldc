#include "harness.h"
#include "lib_timer.h"
#include "HW_CORDIC.h"

/* ---- fake clock: monotonic across the run, so lib_timer's deltas stay right ---- */
static uint64_t g_now_us;
static uint32_t fakeNow_us(void) { return (uint32_t)g_now_us; }
const lib_timer_config_S lib_timer_config = { .getTime_us = fakeNow_us };

void advanceTime_ms(uint32_t ms) { g_now_us += (uint64_t)ms * 1000U; }

/* ---- current-sense front ends: phase shunts at 1.65 V + 0.1 V/A, bus at 0.6 V/A ---- */
#define PHASE_BIAS_V    (1.65f)
#define PHASE_V_PER_A   (0.1f)
#define BUS_V_PER_A     (0.6f)
static const struct { HW_ADC_channels_E ch; uint8_t in; } phaseSense[IO_BRIDGE_PHASE_COUNT] = {
    [IO_BRIDGE_PHASE_U] = { HW_ADC_CHANNEL_1, 6U },
    [IO_BRIDGE_PHASE_V] = { HW_ADC_CHANNEL_2, 7U },
    [IO_BRIDGE_PHASE_W] = { HW_ADC_CHANNEL_1, 8U },
};
#define BUS_CH          (HW_ADC_CHANNEL_2)
#define BUS_IN          (11U)
#define BUS_V_CH        (HW_ADC_CHANNEL_1)   // the bus-voltage divider, 0.15 V/V as on the board
#define BUS_V_IN        (12U)
#define BUS_V_PER_V     (0.15f)
#define PHASE_U_INJ     (0U)                 // U and V at slot 0 of their own ADC's injected sequence
#define PHASE_V_INJ     (0U)
#define PAIR_WINDOW_US  (25U)
#define CREST_PERIOD_US (50U)

static IO_bridge_channelConfig_S        bridgeCfg[IO_BRIDGE_CHANNEL_COUNT];
static IO_bridge_config_S               bridgeConfig;
static app_motorControl_channelConfig_S appCfg[APP_MOTORCONTROL_CHANNEL_COUNT];
static app_motorControl_config_S        appConfig;
static float32_t                        g_profileVelocity;

static void buildConfigs(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR] = (IO_bridge_channelConfig_S){
        .phase = {
            [IO_BRIDGE_PHASE_U] = { .tim = HW_TIM_CHANNEL_PWM_U, .complementPhase = IO_BRIDGE_PHASE_V,
                                    .currentSense = { phaseSense[0].ch, phaseSense[0].in,
                                                      PHASE_U_INJ, PHASE_BIAS_V, PHASE_V_PER_A } },
            [IO_BRIDGE_PHASE_V] = { .tim = HW_TIM_CHANNEL_PWM_V, .complementPhase = IO_BRIDGE_PHASE_U,
                                    .currentSense = { phaseSense[1].ch, phaseSense[1].in,
                                                      PHASE_V_INJ, PHASE_BIAS_V, PHASE_V_PER_A } },
            [IO_BRIDGE_PHASE_W] = { .tim = HW_TIM_CHANNEL_PWM_W, .complementPhase = IO_BRIDGE_PHASE_COUNT,
                                    .currentSense = { phaseSense[2].ch, phaseSense[2].in,
                                                      IO_BRIDGE_INJECTED_NONE, PHASE_BIAS_V, PHASE_V_PER_A } },
        },
        .busCurrent = { BUS_CH, BUS_IN, IO_BRIDGE_INJECTED_NONE, 0.0f, BUS_V_PER_A },
        .injectedPairWindow_us = PAIR_WINDOW_US,
        .zeroOffset = { .filterAlpha = 0.005f, .minSamples = 1000U },
        .busVoltage = { BUS_V_CH, BUS_V_IN, IO_BRIDGE_INJECTED_NONE, 0.0f, BUS_V_PER_V } };
    bridgeConfig = (IO_bridge_config_S){ .channels = bridgeCfg, .numChannels = IO_BRIDGE_CHANNEL_COUNT };

    appCfg[MOTOR_MC] = (app_motorControl_channelConfig_S){
        .gateDriver = GD_MAIN,
        .bridge = MOTOR_BRIDGE,
        .maxVelocity_radPerSec = MAX_VELOCITY,
        .rotorEstimator = APP_ROTORESTIMATION_CHANNEL_MAIN,
        .encoder = ENC_MOTOR,
        .motorPolePairs = 1U,   // the electrical angle is the rotor angle: a test places the field by it
        .pwmPeriod_s = PWM_PERIOD_S,
        .maxVoltage_V = 10.0f,
        .foc = { .maxCurrent_A = FOC_I_MAX, .motorL_H = FOC_L, .motorR_ohm = FOC_R, .currentLoopBandwidth_radPerSec = FOC_BW,
                 .torqueConstant_NmPerA = FOC_KT, .velocityKp_NmPerRadPerSec = FOC_KVP, .velocityKi_NmPerRad = FOC_KVI,
                 .velocityKff = 0.0f,
                 .positionKp_perSec = POS_KPP, .positionKi_perSec2 = POS_KPI, .maxPosition_rad = POS_MAX },
        .defaultMode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
        .prechargeTime_ms = PRECHARGE_MS,
        .alignmentDuty01 = ALIGN_DUTY_01,
        .alignmentDwell_ms = 500U,
        .overcurrentPhaseTrip_A = PHASE_TRIP_A,
        .overcurrentBusTrip_A = BUS_TRIP_A,
        .encoderFaultLimit = ENCODER_FAULT_LIMIT };
    appConfig = (app_motorControl_config_S){ .channels = appCfg, .numChannels = APP_MOTORCONTROL_CHANNEL_COUNT };
}

app_motorControl_channelConfig_S * freshConfig(void)
{
    buildConfigs();
    return &appCfg[MOTOR_MC];
}

bool reinit(void) { return app_motorControl_init(&appConfig); }

/* ---- inputs ---- */

// The bridge stamps each crest pair off the fake clock, one PWM period on per call.
void setPhaseCurrentsUV(float32_t ampsU, float32_t ampsV)
{
    g_now_us += CREST_PERIOD_US;
    mock_HW_ADC_setInjectedVolts(phaseSense[IO_BRIDGE_PHASE_U].ch, PHASE_U_INJ, PHASE_BIAS_V + (ampsU * PHASE_V_PER_A));
    mock_HW_ADC_setInjectedVolts(phaseSense[IO_BRIDGE_PHASE_V].ch, PHASE_V_INJ, PHASE_BIAS_V + (ampsV * PHASE_V_PER_A));
    mock_HW_ADC_fireInjected(phaseSense[IO_BRIDGE_PHASE_U].ch, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_fireInjected(phaseSense[IO_BRIDGE_PHASE_V].ch, HW_ADC_CONVERSION_STATUS_OK);
}

void setBusCurrent(float32_t amps) { mock_HW_ADC_setVolts(BUS_CH, BUS_IN, amps * BUS_V_PER_A); }
void setBusVoltage(float32_t volts) { mock_HW_ADC_setVolts(BUS_V_CH, BUS_V_IN, volts * BUS_V_PER_V); }

void setNominalCurrents(void)
{
    setPhaseCurrentsUV(0.0f, 0.0f);
    setBusCurrent(0.0f);
}

/* ---- the loop ---- */

void cycle(void) { app_motorControl_run50us(MOTOR_BRIDGE); }

// On the board twenty cycles run between passes: those before a pass fetch what it
// latches on, those after apply what it decided. One of each stands for them.
void tick(void)
{
    const float32_t before = snapshot().positionTarget_rad;
    cycle();
    app_motorControl_run1ms();
    cycle();
    g_profileVelocity = (snapshot().positionTarget_rad - before) / 1.0e-3f;
}

void alignAtZero(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    tick();          // precharging
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();          // aligning
    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();          // the dwell expires: offset 0
}

void driveToRunning(float32_t velocity)
{
    alignAtZero();
    app_motorControl_setVelocity(MOTOR_MC, velocity, 0.0f);
    tick();          // commutating
}

void commutateAt(float32_t rotorDeg, float32_t velocity)
{
    mock_app_rotorEstimation_setAngle(DEG2RAD(rotorDeg));
    app_motorControl_setVelocity(MOTOR_MC, velocity, 0.0f);
    tick();
}

void startMethodAt(app_motorControl_mode_E mode, float32_t rotorDeg, float32_t velocity)
{
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    app_motorControl_setMode(MOTOR_MC, mode);
    app_motorControl_setVelocity(MOTOR_MC, velocity, 0.0f);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    commutateAt(rotorDeg, velocity);
}

/* ---- observation ---- */

app_motorControl_snapshot_S snapshot(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s;
}

float32_t profileVelocity(void) { return g_profileVelocity; }

bool bridgeEnabled(void) { return mock_HW_TIM_getMoe(BRIDGE_PERIPH); }

static HW_TIM_channels_E timChannel(IO_bridge_phase_E phase)
{
    static const HW_TIM_channels_E channels[IO_BRIDGE_PHASE_COUNT] = { HW_TIM_CHANNEL_PWM_U, HW_TIM_CHANNEL_PWM_V, HW_TIM_CHANNEL_PWM_W };
    return channels[phase];
}

bool phaseFloating(IO_bridge_phase_E phase) { return !mock_HW_TIM_getOutputEnabled(timChannel(phase)); }
uint32_t phaseCompare(IO_bridge_phase_E phase) { return mock_HW_TIM_getCompare(timChannel(phase)); }

uint32_t drivenCompare(void)
{
    uint32_t maxCompare = 0U;
    for (size_t p = 0U; p < IO_BRIDGE_PHASE_COUNT; p++)
    {
        if (!phaseFloating((IO_bridge_phase_E)p))
        {
            const uint32_t c = phaseCompare((IO_bridge_phase_E)p);
            if (c > maxCompare) { maxCompare = c; }
        }
    }
    return maxCompare;
}

int observedSector(void)
{
    IO_bridge_phase_E dutyD = IO_BRIDGE_PHASE_COUNT;
    IO_bridge_phase_E floatP = IO_BRIDGE_PHASE_COUNT;
    for (size_t p = 0U; p < IO_BRIDGE_PHASE_COUNT; p++)
    {
        const IO_bridge_phase_E phase = (IO_bridge_phase_E)p;
        if (phaseFloating(phase)) { floatP = phase; }
        else if (phaseCompare(phase) > 0U) { dutyD = phase; }
    }

    // (duty-d phase, floating phase) for U+V-, U+W-, V+U-, V+W-, W+V-, W+U- (fw~mc_011)
    static const struct { IO_bridge_phase_E dutyD; IO_bridge_phase_E floatP; } table[6] = {
        { IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W },
        { IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V },
        { IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U },
        { IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W },
        { IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V },
        { IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U },
    };
    int sector = -1;
    for (int i = 0; i < 6; i++)
    {
        if ((table[i].dutyD == dutyD) && (table[i].floatP == floatP)) { sector = i; }
    }
    return sector;
}

bool allPhasesAtZeroDuty(void)
{
    return (phaseCompare(IO_BRIDGE_PHASE_U) == 0U) &&
           (phaseCompare(IO_BRIDGE_PHASE_V) == 0U) &&
           (phaseCompare(IO_BRIDGE_PHASE_W) == 0U);
}

// The zero-sequence offset cancels in the duty differences (fw~mc_014).
statorVoltage_S statorVoltage(void)
{
    const float32_t dU = (float32_t)phaseCompare(IO_BRIDGE_PHASE_U) / (float32_t)PERIOD;
    const float32_t dV = (float32_t)phaseCompare(IO_BRIDGE_PHASE_V) / (float32_t)PERIOD;
    const float32_t dW = (float32_t)phaseCompare(IO_BRIDGE_PHASE_W) / (float32_t)PERIOD;
    const float32_t vAlpha = TEST_VBUS_V * ((2.0f * dU) - dV - dW) / 3.0f;
    const float32_t vBeta  = TEST_VBUS_V * (dV - dW) / SQRT3_F;
    const statorVoltage_S v = { .angleDeg = atan2f(vBeta, vAlpha) * 180.0f / 3.14159265f,
                                .magnitude_V = sqrtf((vAlpha * vAlpha) + (vBeta * vBeta)) };
    return v;
}

rotorVoltage_S rotorVoltageAt(float32_t rotorDeg)
{
    const statorVoltage_S stator = statorVoltage();
    const float32_t rel = DEG2RAD(stator.angleDeg - rotorDeg);
    const rotorVoltage_S v = { .vD = stator.magnitude_V * cosf(rel), .vQ = stator.magnitude_V * sinf(rel) };
    return v;
}

float32_t wrapDeg(float32_t deg)
{
    while (deg > 180.0f)  { deg -= 360.0f; }
    while (deg <= -180.0f) { deg += 360.0f; }
    return deg;
}

// K_i T a step for any i_q* the outer integrals still ask, with the current at zero.
float32_t qVoltageDriftAfterTheLimit(uint32_t steps)
{
    setBusVoltage(TEST_VBUS_V);
    app_motorControl_run1ms();
    cycle();
    const float32_t first = rotorVoltageAt(0.0f).vQ;
    for (uint32_t n = 0U; n < steps; n++) { cycle(); }
    return rotorVoltageAt(0.0f).vQ - first;
}

/* ---- Unity fixtures ---- */

static const HW_CORDIC_config_S cordicConfig = { 0 };

void setUp(void)
{
    (void)HW_CORDIC_init(&cordicConfig);
    mock_HW_TIM_reset(PERIOD);
    mock_HW_ADC_reset();
    mock_IO_AS5048_reset();
    mock_app_rotorEstimation_reset();
    mock_dev_gateDriver_reset();
    buildConfigs();
    (void)IO_bridge_init(&bridgeConfig);
    (void)app_motorControl_init(&appConfig);
    setNominalCurrents();
    setBusVoltage(TEST_VBUS_V);
    g_profileVelocity = 0.0f;
}

void tearDown(void) {}
