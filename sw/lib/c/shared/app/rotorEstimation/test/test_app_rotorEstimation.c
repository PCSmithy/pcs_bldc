// app_rotorEstimation over the IO_AS5048 double. The spec's numbers: omega_n
// = 2*pi*50 rad/s, zeta = 1, p_d = 157 rad/s, Ts = 1 ms, Tpwm = 50 us, J =
// 1.45e-4 kg m^2, b = Tc = 0, so K = [l1, l2, l3] * Ts = [0.7853, 197.34,
// 2.2468]. Effects are read through the public state getter.
#include "app_rotorEstimation.h"
#include "IO_AS5048.h"
#include "mock_IO_AS5048.h"
#include "unity.h"

#include <math.h>

#define PI_D        (3.14159265358979323846)   // <math.h> hides M_PI under -std=c11
#define PI_F        (3.14159265f)
#define OMEGA_N     (2.0f * PI_F * 50.0f)
#define ZETA        (1.0f)
#define PD          (157.0f)
#define TS          (0.001f)
#define TPWM        (50.0e-6f)
#define INERTIA     (1.45e-4f)
#define VISCOUS     (7.2e-4f)    // Nm per rad/s, the friction tests' b
#define COULOMB     (0.01f)      // Nm, the friction tests' Tc
#define OMEGA_DB    (2.0f)       // rad/s, where the Coulomb term reaches +-Tc
#define CYCLES_PER_MS (20U)
#define RESEED_AFTER  (5U)       // invalid reads, the board's encoder-fault limit
#define L1_TS       (((2.0f * ZETA * OMEGA_N) + PD) * TS)
#define L2_TS       (((OMEGA_N * OMEGA_N) + (2.0f * ZETA * OMEGA_N * PD)) * TS)
#define L3_TS       (INERTIA * OMEGA_N * OMEGA_N * PD * TS)

static app_rotorEstimation_channelConfig_S channelConfig[APP_ROTORESTIMATION_CHANNEL_COUNT];
static app_rotorEstimation_config_S config;

static void nominalConfig(void)
{
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].encoder = IO_AS5048_CHANNEL_MOTOR;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].naturalFrequency_radPerSec = OMEGA_N;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].dampingRatio = ZETA;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].correctionPeriod_s = TS;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].estimatePeriod_s = TPWM;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].inertia_kgm2 = INERTIA;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].viscousFriction_NmPerRadPerSec = 0.0f;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].coulombFriction_Nm = 0.0f;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].disturbancePole_radPerSec = PD;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].coulombRampVelocity_radPerSec = OMEGA_DB;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].reseedAfterInvalidReads = RESEED_AFTER;
    config.channels = channelConfig;
    config.numChannels = APP_ROTORESTIMATION_CHANNEL_COUNT;
}

void setUp(void)
{
    mock_IO_AS5048_reset();
    nominalConfig();
    TEST_ASSERT_TRUE(app_rotorEstimation_init(&config));
}

void tearDown(void) {}

static float32_t theta(void)
{
    float32_t t = 0.0f;
    float32_t w = 0.0f;
    float32_t d = 0.0f;
    app_rotorEstimation_getState(APP_ROTORESTIMATION_CHANNEL_MAIN, &t, &w, &d);
    return t;
}

static float32_t omega(void)
{
    float32_t t = 0.0f;
    float32_t w = 0.0f;
    float32_t d = 0.0f;
    app_rotorEstimation_getState(APP_ROTORESTIMATION_CHANNEL_MAIN, &t, &w, &d);
    return w;
}

static float32_t torqueHat(void)
{
    float32_t t = 0.0f;
    float32_t w = 0.0f;
    float32_t d = 0.0f;
    app_rotorEstimation_getState(APP_ROTORESTIMATION_CHANNEL_MAIN, &t, &w, &d);
    return d;
}

static void cycles(uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        app_rotorEstimation_run50us();
    }
}

// One millisecond: the encoder reads `angle_rad`, then the PWM cycles run.
static void tick(float32_t angle_rad)
{
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, angle_rad);
    app_rotorEstimation_run1ms();
    cycles(CYCLES_PER_MS);
}

static float32_t wrap2pi(float32_t a)
{
    return (float32_t)fmod((double)a + (4.0 * PI_D), 2.0 * PI_D);
}

// Signed difference of two angles in [0, 2pi), wrapped to (-pi, pi].
static float32_t wrapPi(float32_t a)
{
    return (float32_t)(fmod((double)a + (5.0 * PI_D), 2.0 * PI_D) - PI_D);
}

// Seed at rest at `angle_rad`, then a read `e` past it, applied by one cycle.
static void seedThenCorrect(float32_t angle_rad, float32_t e)
{
    tick(angle_rad);
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, angle_rad + e);
    app_rotorEstimation_run1ms();
    cycles(1U);
}

/* ---- init ---- */

static void test_init_rejects_bad_config(void)
{
    nominalConfig();
    channelConfig[0].encoder = IO_AS5048_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].naturalFrequency_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].dampingRatio = 0.4f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].disturbancePole_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].naturalFrequency_radPerSec = 450.0f;   // 2*zeta*omega_n*Ts = 0.9, plus p_d*Ts = 1.057
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].estimatePeriod_s = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].estimatePeriod_s = 2.0f * TS;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].inertia_kgm2 = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].viscousFriction_NmPerRadPerSec = -1.0e-4f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].coulombFriction_Nm = -1.0e-3f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].coulombRampVelocity_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].coulombRampVelocity_radPerSec = -1.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    TEST_ASSERT_FALSE(app_rotorEstimation_init(NULL));
}

static void test_state_is_zero_before_the_first_valid_read(void)
{
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_IDLE);
    tick(1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());
}

/* ---- [test->fw~est_rotor_001~1] correction ---- */

static void test_first_valid_read_seeds_angle_with_zero_velocity_and_torque(void)
{
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, 1.0f);
    app_rotorEstimation_run1ms();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, theta());   // takes effect by the next cycle
    cycles(1U);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());
}

static void test_valid_read_applies_gain_times_error_by_the_next_cycle(void)
{
    tick(1.0f);                                        // seeded at 1.0, at rest
    const float32_t e = 0.01f;
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, 1.0f + e);
    app_rotorEstimation_run1ms();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());    // not yet
    cycles(1U);
    const float32_t expTorque = L3_TS * e;
    // Net of the one cycle advance: T_d acts on omega through Tpwm/J and on
    // theta through Tpwm^2/2J, omega on theta through Tpwm.
    const float32_t expOmega = (L2_TS * e) + ((expTorque * TPWM) / INERTIA);
    const float32_t expTheta = 1.0f + (L1_TS * e) + ((L2_TS * e) * TPWM) + ((expTorque * TPWM * TPWM) / (2.0f * INERTIA));
    // Tolerances under the advance terms (7.7e-3 rad/s, 9.9e-5 rad), so an
    // advance applied before the correction, or skipped, fails here.
    TEST_ASSERT_FLOAT_WITHIN(0.01f * expTorque, expTorque, torqueHat());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expOmega, omega());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, expTheta, theta());
}

static void test_error_is_wrapped_across_two_pi(void)
{
    tick(0.01f);                                       // seeded just past zero
    const float32_t e = -0.02f;                        // the encoder reads 0.01 below 2*pi
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, (2.0f * PI_F) - 0.01f);
    app_rotorEstimation_run1ms();
    cycles(1U);
    const float32_t expOmega = (L2_TS * e) + ((L3_TS * e * TPWM) / INERTIA);
    // 0.1 %: the error is the difference of two angles near 2*pi in float
    TEST_ASSERT_FLOAT_WITHIN(1e-3f * fabsf(expOmega), expOmega, omega());
}

static void test_invalid_read_skips_the_correction(void)
{
    tick(1.0f);
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_FAULT);
    tick(2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());

    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_IDLE);
    tick(2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());
}

// Seed at rest at 1.0 rad, then `outage` ticks with invalid reads.
static void seedThenOutage(uint32_t outage)
{
    tick(1.0f);
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_FAULT);
    for (uint32_t ms = 0U; ms < outage; ms++)
    {
        tick(1.0f);
    }
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_OK);
}

static void test_valid_read_after_an_outage_past_the_limit_reseeds(void)
{
    seedThenOutage(RESEED_AFTER + 1U);
    tick(1.0f + 3.0f);                                 // an error near pi
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());
}

static void test_valid_read_after_an_outage_at_the_limit_corrects(void)
{
    seedThenOutage(RESEED_AFTER);
    const float32_t e = 0.01f;
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, 1.0f + e);
    app_rotorEstimation_run1ms();
    cycles(1U);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * L3_TS * e, L3_TS * e, torqueHat());
}

static void test_reseed_is_reposted_until_it_lands(void)
{
    seedThenOutage(RESEED_AFTER + 1U);
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, 4.0f);
    app_rotorEstimation_run1ms();                      // no cycle consumes this one
    app_rotorEstimation_run1ms();
    cycles(1U);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());
}

static void test_velocity_converges_within_1pct_inside_50ms_and_holds_through_wraps(void)
{
    const float32_t rate = 21.0f;   // rad/s, about 3.3 turns per second
    float32_t angle = 0.0f;
    tick(angle);                    // seed at rest
    for (uint32_t ms = 1U; ms <= 1050U; ms++)
    {
        angle = wrap2pi(angle + (rate * TS));
        tick(angle);
        if (ms >= 50U)
        {
            TEST_ASSERT_FLOAT_WITHIN(0.01f * rate, rate, omega());
            TEST_ASSERT_TRUE((theta() >= 0.0f) && (theta() < 2.0f * PI_F));
        }
    }
}

static void test_constant_acceleration_lands_in_the_torque_state(void)
{
    // From rest under 50 rad/s^2 with no torque input: the disturbance state
    // absorbs J*a and the angle error closes to under 0.01 deg per tick.
    const float32_t accel = 50.0f;
    tick(0.0f);                     // seed at rest, t = 0
    for (uint32_t ms = 1U; ms <= 500U; ms++)
    {
        const float32_t t = (float32_t)ms * TS;
        const float32_t angle = wrap2pi(0.5f * accel * t * t);
        const float32_t e = wrapPi(angle - theta());   // what this tick's correction sees
        tick(angle);
        if (ms >= 200U)
        {
            TEST_ASSERT_FLOAT_WITHIN(0.01f * (PI_F / 180.0f), 0.0f, e);
            TEST_ASSERT_FLOAT_WITHIN(0.02f * INERTIA * accel, INERTIA * accel, torqueHat());
        }
    }
}

/* ---- [test->fw~est_rotor_002~1] per-cycle advance ---- */

static void test_cycles_advance_the_angle_by_velocity_times_period(void)
{
    const float32_t rate = 21.0f;
    float32_t angle = 0.0f;
    tick(angle);
    for (uint32_t ms = 1U; ms <= 100U; ms++)
    {
        angle = wrap2pi(angle + (rate * TS));
        tick(angle);
    }
    const float32_t w = omega();
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_FAULT);
    app_rotorEstimation_run1ms();                      // no valid read
    const float32_t start = theta();
    cycles(CYCLES_PER_MS);
    const float32_t expected = wrap2pi(start + (20.0f * w * TPWM));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expected, theta());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, w, omega());
}

static void test_torque_input_accelerates_the_estimate_from_rest(void)
{
    tick(0.0f);                                        // seeded at rest, T_d zero
    const float32_t tau = 0.01f;
    app_rotorEstimation_setTorqueInput(APP_ROTORESTIMATION_CHANNEL_MAIN, tau);
    cycles(CYCLES_PER_MS);                             // no read in between
    const float32_t expOmega = (20.0f * tau * TPWM) / INERTIA;
    const float32_t expTheta = (200.0f * tau * TPWM * TPWM) / INERTIA;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * expOmega, expOmega, omega());
    TEST_ASSERT_FLOAT_WITHIN(0.01f * expTheta, expTheta, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, torqueHat());
}

static void test_torque_state_accelerates_the_estimate_and_holds(void)
{
    seedThenCorrect(1.0f, 0.01f);                      // leaves T_d = l3 * e * Ts
    const float32_t torque = torqueHat();
    const float32_t w = omega();
    TEST_ASSERT_TRUE(torque > 0.0f);
    cycles(CYCLES_PER_MS);                             // no read in between
    const float32_t expAdvance = (20.0f * torque * TPWM) / INERTIA;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * expAdvance, w + expAdvance, omega());
    TEST_ASSERT_EQUAL_FLOAT(torque, torqueHat());
}

// Track `rate` for 300 ms under the torque input `tau` with b and Tc configured
// (the spin-up through the Coulomb ramp leaves T_d a residual that takes that
// long to clear), then hold 20 read-less cycles: the velocity estimate's drift.
static float32_t frictionHoldDrift(float32_t rate, float32_t tau)
{
    mock_IO_AS5048_reset();
    nominalConfig();
    channelConfig[0].viscousFriction_NmPerRadPerSec = VISCOUS;
    channelConfig[0].coulombFriction_Nm = COULOMB;
    TEST_ASSERT_TRUE(app_rotorEstimation_init(&config));

    app_rotorEstimation_setTorqueInput(APP_ROTORESTIMATION_CHANNEL_MAIN, tau);
    double angle = 0.0;   // accumulated in double: a float accumulator drifts the rate
    tick(0.0f);
    for (uint32_t ms = 1U; ms <= 300U; ms++)
    {
        angle = fmod(angle + ((double)rate * (double)TS) + (4.0 * PI_D), 2.0 * PI_D);
        tick((float32_t)angle);
    }
    const float32_t w = omega();
    TEST_ASSERT_FLOAT_WITHIN(0.01f * fabsf(rate), rate, w);
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_FAULT);
    app_rotorEstimation_run1ms();                      // no valid read
    cycles(CYCLES_PER_MS);
    return omega() - w;
}

static void test_torque_matching_the_friction_holds_the_velocity(void)
{
    // Past omega_db the Coulomb term is +-Tc: a torque input of b*omega +
    // Tc*sgn(omega) is the friction's match in either direction.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, frictionHoldDrift(21.0f, (VISCOUS * 21.0f) + COULOMB));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, frictionHoldDrift(-21.0f, (VISCOUS * -21.0f) - COULOMB));
}

static void test_torque_matching_the_ramped_friction_holds_the_velocity_inside_the_band(void)
{
    // Inside +-omega_db the Coulomb term is Tc*omega/omega_db: the matching
    // input is linear in the speed, in either direction.
    const float32_t rate = OMEGA_DB / 2.0f;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, frictionHoldDrift(rate, (VISCOUS * rate) + (COULOMB * rate / OMEGA_DB)));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, frictionHoldDrift(-rate, (VISCOUS * -rate) - (COULOMB * rate / OMEGA_DB)));
}

static void test_advance_wraps_the_angle_to_2pi(void)
{
    tick((2.0f * PI_F) - 1e-3f);                       // seeded just below the wrap
    float32_t angle = (2.0f * PI_F) - 1e-3f;
    for (uint32_t ms = 1U; ms <= 60U; ms++)
    {
        angle = wrap2pi(angle + (21.0f * TS));
        tick(angle);
        TEST_ASSERT_TRUE((theta() >= 0.0f) && (theta() < 2.0f * PI_F));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_bad_config);
    RUN_TEST(test_state_is_zero_before_the_first_valid_read);
    RUN_TEST(test_first_valid_read_seeds_angle_with_zero_velocity_and_torque);
    RUN_TEST(test_valid_read_applies_gain_times_error_by_the_next_cycle);
    RUN_TEST(test_error_is_wrapped_across_two_pi);
    RUN_TEST(test_invalid_read_skips_the_correction);
    RUN_TEST(test_valid_read_after_an_outage_past_the_limit_reseeds);
    RUN_TEST(test_valid_read_after_an_outage_at_the_limit_corrects);
    RUN_TEST(test_reseed_is_reposted_until_it_lands);
    RUN_TEST(test_velocity_converges_within_1pct_inside_50ms_and_holds_through_wraps);
    RUN_TEST(test_constant_acceleration_lands_in_the_torque_state);
    RUN_TEST(test_cycles_advance_the_angle_by_velocity_times_period);
    RUN_TEST(test_torque_input_accelerates_the_estimate_from_rest);
    RUN_TEST(test_torque_state_accelerates_the_estimate_and_holds);
    RUN_TEST(test_torque_matching_the_friction_holds_the_velocity);
    RUN_TEST(test_torque_matching_the_ramped_friction_holds_the_velocity_inside_the_band);
    RUN_TEST(test_advance_wraps_the_angle_to_2pi);
    return UNITY_END();
}
