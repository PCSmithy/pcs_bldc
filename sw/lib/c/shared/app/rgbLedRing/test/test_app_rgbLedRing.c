#include "app_rgbLedRing.h"
#include "unity.h"

// The board ring; the render core takes the count as a parameter.
#define LED_COUNT   36U
#define LVL         ((uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS)
#define FULL_SCALE  (10.0f)   // speedo full-scale speed (rad/s) for the tests

static app_rgbLedRing_state_S       state;
static app_rgbLedRing_rgb_S         pixels[LED_COUNT];
static app_motorControl_snapshot_S  motor;

void setUp(void)
{
    app_rgbLedRing_renderInit(&state);
    app_rgbLedRing_seedEncoders(&state, 0.0f, 0.0f);
    for (uint16_t i = 0U; i < LED_COUNT; i++)
    {
        pixels[i] = (app_rgbLedRing_rgb_S){ 0U, 0U, 0U };
    }
    motor = (app_motorControl_snapshot_S){ .state = APP_MOTORCONTROL_STATE_ENABLED };
}

void tearDown(void) {}

static void render(float32_t dialDeg, float32_t motorDeg)
{
    app_rgbLedRing_renderFrame(&state, dialDeg, motorDeg, &motor, FULL_SCALE, pixels, LED_COUNT);
}

/* ---- fw~obs_ring_001: mode selection ---- */

// [test->fw~obs_ring_001~1]
static void test_starts_in_position_mode(void)
{
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_POSITION, state.mode);
}

// [test->fw~obs_ring_001~1]
static void test_advance_toggles_position_and_speedo(void)
{
    app_rgbLedRing_advanceMode(&state);
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_SPEEDO, state.mode);
    app_rgbLedRing_advanceMode(&state);
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_POSITION, state.mode);
}

/* ---- fw~obs_ring_002: state display + per-mode rendering ---- */

// [test->fw~obs_ring_002~1] [test->fw~mc_009~1]
static void test_off_when_motor_disabled(void)
{
    motor.state = APP_MOTORCONTROL_STATE_DISABLED;
    render(90.0f, 250.0f);
    for (uint16_t i = 0U; i < LED_COUNT; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0U, pixels[i].red);
        TEST_ASSERT_EQUAL_UINT8(0U, pixels[i].green);
        TEST_ASSERT_EQUAL_UINT8(0U, pixels[i].blue);
    }
}

// [test->fw~obs_ring_002~1] [test->fw~mc_009~1]
static void test_red_takeover_when_motor_faulted(void)
{
    motor.state = APP_MOTORCONTROL_STATE_FAULTED;
    render(90.0f, 250.0f);
    for (uint16_t i = 0U; i < LED_COUNT; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(LVL, pixels[i].red);
        TEST_ASSERT_EQUAL_UINT8(0U,  pixels[i].green);
        TEST_ASSERT_EQUAL_UINT8(0U,  pixels[i].blue);
    }
}

// [test->fw~obs_ring_002~1]
// POSITION: a green pip tracks the motor angle, a blue pip the dial angle, each
// complemented into the ring's (reversed) LED order.
static void test_position_pips_track_motor_and_dial(void)
{
    state.mode = APP_RGBLEDRING_MODE_POSITION;

    // motor at 0 deg -> ring LED 0; dial at 180 deg -> ring LED 18.
    render(180.0f, 0.0f);

    TEST_ASSERT_EQUAL_UINT8(LVL, pixels[0].green);   // motor pip (green)
    TEST_ASSERT_EQUAL_UINT8(0U,  pixels[0].red);
    TEST_ASSERT_EQUAL_UINT8(0U,  pixels[0].blue);

    TEST_ASSERT_EQUAL_UINT8(LVL, pixels[18].blue);   // dial pip (blue)
    TEST_ASSERT_EQUAL_UINT8(0U,  pixels[18].red);
}

// [test->fw~obs_ring_002~1]
// SPEEDO: setpoint needle (amber) sits at its full-scale sweep; the scaffold
// actual-speed needle (green) sits at zero on the first, still frame.
static void test_speedo_needles_track_setpoint_and_actual(void)
{
    state.mode = APP_RGBLEDRING_MODE_SPEEDO;
    motor.velocitySetpoint_radPerSec = FULL_SCALE;   // norm +1 -> +150 deg, complemented -> LED 21

    render(0.0f, 0.0f);   // motor still -> actual estimate 0 -> LED 0

    TEST_ASSERT_EQUAL_UINT8(LVL, pixels[21].red);    // setpoint needle magenta
    TEST_ASSERT_EQUAL_UINT8(0U,  pixels[21].green);
    TEST_ASSERT_EQUAL_UINT8(LVL, pixels[21].blue);

    TEST_ASSERT_EQUAL_UINT8(LVL, pixels[0].green);   // actual needle green at zero
    TEST_ASSERT_EQUAL_UINT8(0U,  pixels[0].red);
    TEST_ASSERT_EQUAL_UINT8(0U,  pixels[0].blue);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_starts_in_position_mode);
    RUN_TEST(test_advance_toggles_position_and_speedo);

    RUN_TEST(test_off_when_motor_disabled);
    RUN_TEST(test_red_takeover_when_motor_faulted);
    RUN_TEST(test_position_pips_track_motor_and_dial);
    RUN_TEST(test_speedo_needles_track_setpoint_and_actual);

    return UNITY_END();
}
