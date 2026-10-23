// app_rotorEstimation over the IO_AS5048 double. The spec's numbers: omega_n
// = 2*pi*50 rad/s, zeta = 1, Ts = 1 ms, Tpwm = 50 us, so K = [Kp*Ts, Ki*Ts]
// = [0.62832, 98.696]. Effects are read through the public state getter.
#include "app_rotorEstimation.h"
#include "IO_AS5048.h"
#include "mock_IO_AS5048.h"
#include "unity.h"

#include <math.h>

#define PI_D        (3.14159265358979323846)   // <math.h> hides M_PI under -std=c11
#define PI_F        (3.14159265f)
#define OMEGA_N     (2.0f * PI_F * 50.0f)
#define ZETA        (1.0f)
#define TS          (0.001f)
#define TPWM        (50.0e-6f)
#define CYCLES_PER_MS (20U)
#define KP_TS       (2.0f * ZETA * OMEGA_N * TS)
#define KI_TS       (OMEGA_N * OMEGA_N * TS)

static app_rotorEstimation_channelConfig_S channelConfig[APP_ROTORESTIMATION_CHANNEL_COUNT];
static app_rotorEstimation_config_S config;

static void nominalConfig(void)
{
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].encoder = IO_AS5048_CHANNEL_MOTOR;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].omega_n = OMEGA_N;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].zeta = ZETA;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].correctionPeriod_s = TS;
    channelConfig[APP_ROTORESTIMATION_CHANNEL_MAIN].estimatePeriod_s = TPWM;
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
    app_rotorEstimation_getState(APP_ROTORESTIMATION_CHANNEL_MAIN, &t, &w);
    return t;
}

static float32_t omega(void)
{
    float32_t t = 0.0f;
    float32_t w = 0.0f;
    app_rotorEstimation_getState(APP_ROTORESTIMATION_CHANNEL_MAIN, &t, &w);
    return w;
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

/* ---- init ---- */

static void test_init_rejects_bad_config(void)
{
    nominalConfig();
    channelConfig[0].encoder = IO_AS5048_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].omega_n = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].zeta = 0.4f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].omega_n = 600.0f;   // 2*zeta*omega_n*Ts = 1.2
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].estimatePeriod_s = 0.0f;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    nominalConfig();
    channelConfig[0].estimatePeriod_s = 2.0f * TS;
    TEST_ASSERT_FALSE(app_rotorEstimation_init(&config));

    TEST_ASSERT_FALSE(app_rotorEstimation_init(NULL));
}

static void test_state_is_zero_before_the_first_valid_read(void)
{
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_IDLE);
    tick(1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
}

/* ---- [test->fw~est_rotor_001~1] correction ---- */

static void test_first_valid_read_seeds_angle_with_zero_velocity(void)
{
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, 1.0f);
    app_rotorEstimation_run1ms();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, theta());   // takes effect by the next cycle
    cycles(1U);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
}

static void test_valid_read_applies_gain_times_error_by_the_next_cycle(void)
{
    tick(1.0f);                                        // seeded at 1.0, at rest
    const float32_t e = 0.01f;
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, 1.0f + e);
    app_rotorEstimation_run1ms();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());    // not yet
    cycles(1U);
    const float32_t expOmega = KI_TS * e;
    const float32_t expTheta = 1.0f + (KP_TS * e) + (expOmega * TPWM);   // net of the cycle advance
    TEST_ASSERT_FLOAT_WITHIN(0.01f * expOmega, expOmega, omega());
    TEST_ASSERT_FLOAT_WITHIN(0.01f * KP_TS * e, expTheta, theta());
}

static void test_error_is_wrapped_across_two_pi(void)
{
    tick(0.1f);                                        // seeded just past zero
    const float32_t e = -0.02f;                        // the encoder reads just below 2*pi
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_MOTOR, (2.0f * PI_F) + 0.1f + e);
    app_rotorEstimation_run1ms();
    cycles(1U);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * fabsf(KI_TS * e), KI_TS * e, omega());
}

static void test_invalid_read_skips_the_correction(void)
{
    tick(1.0f);
    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_FAULT);
    tick(2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());

    mock_IO_AS5048_setStatus(IO_AS5048_CHANNEL_MOTOR, IO_AS5048_STATUS_IDLE);
    tick(2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, theta());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, omega());
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
    TEST_ASSERT_EQUAL_FLOAT(w, omega());
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
    RUN_TEST(test_first_valid_read_seeds_angle_with_zero_velocity);
    RUN_TEST(test_valid_read_applies_gain_times_error_by_the_next_cycle);
    RUN_TEST(test_error_is_wrapped_across_two_pi);
    RUN_TEST(test_invalid_read_skips_the_correction);
    RUN_TEST(test_velocity_converges_within_1pct_inside_50ms_and_holds_through_wraps);
    RUN_TEST(test_cycles_advance_the_angle_by_velocity_times_period);
    RUN_TEST(test_advance_wraps_the_angle_to_2pi);
    return UNITY_END();
}
