// Unit tests for app_userControls: taps and dial movement against the motor
// double's snapshot, each pass one app_userControls_run1ms on a 1 ms fake clock.

#include "app_userControls.h"
#include "mock_app_motorControl.h"
#include "mock_IO_AS5048.h"
#include "lib_timer.h"
#include "lib_utils.h"
#include "unity.h"

#define TEST_MAX_VEL (18.0f)

/* ---- seams ---- */

static uint32_t fakeCounter_us;
static uint32_t fakeTime_us(void)
{
    return fakeCounter_us;
}
const lib_timer_config_S lib_timer_config = { .getTime_us = fakeTime_us };

static bool buttonDown;
bool dev_switch_isActive(dev_switch_channel_E channel)
{
    (void)channel;
    return buttonDown;
}

static uint32_t cycleModeCalls;
void app_rgbLedRing_cycleMode(app_rgbLedRing_channel_E channel)
{
    (void)channel;
    cycleModeCalls++;
}

static const app_userControls_config_S config = {
    .motor = APP_MOTORCONTROL_CHANNEL_MAIN,
    .button = DEV_SWITCH_CHANNEL_USER_BUTTON,
    .dial = IO_AS5048_CHANNEL_DIAL,
    .ring = APP_RGBLEDRING_CHANNEL_RING,
};

/* ---- helpers ---- */

static void passes(uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        fakeCounter_us += 1000U;
        app_userControls_run1ms();
    }
}

// A lone tap: a 50 ms press, then the double-tap window left to elapse.
static void tap(void)
{
    buttonDown = true;
    passes(50U);
    buttonDown = false;
    passes(400U);
}

static void setMotor(app_motorControl_mode_E mode, app_motorControl_state_E state, app_motorControl_mode_E selected)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.mode = mode;
    snap.state = state;
    snap.selectedMode = selected;
    mock_app_motorControl_setSnapshot(&snap);
}

static void setDial_deg(float32_t deg)
{
    mock_IO_AS5048_setAngle(IO_AS5048_CHANNEL_DIAL, DEG_TO_RAD(deg));
}

static uint32_t targetWrites(void)
{
    return mock_app_motorControl_setVelocityCalls + mock_app_motorControl_setPositionSetpointCalls +
           mock_app_motorControl_setTorqueCalls;
}

void setUp(void)
{
    mock_app_motorControl_reset();
    mock_IO_AS5048_reset();
    buttonDown = false;
    cycleModeCalls = 0U;
    setDial_deg(30.0f);
    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_DISABLED, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    TEST_ASSERT_TRUE(app_userControls_init(&config));
}

void tearDown(void) {}

/* ---- taps ---- */

// Idle passes write nothing to the motor control.
static void test_idle_passes_write_nothing(void)
{
    passes(100U);
    setDial_deg(120.0f);
    passes(100U);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, targetWrites());
}

// The first tap on a host-started drive stops it, writing no target.
static void test_the_first_tap_stops_a_host_started_drive(void)
{
    setMotor(APP_MOTORCONTROL_MODE_FOC_VELOCITY, APP_MOTORCONTROL_STATE_ENABLED, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    passes(10U);
    tap();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, mock_app_motorControl_lastMode);
    TEST_ASSERT_EQUAL_UINT32(0U, targetWrites());
}

// A drive requested but not yet enabled (precharging) also stops on a tap.
static void test_a_tap_stops_a_requested_drive_before_its_enable(void)
{
    setMotor(APP_MOTORCONTROL_MODE_FOC_POSITION, APP_MOTORCONTROL_STATE_DISABLED, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tap();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, mock_app_motorControl_lastMode);
    TEST_ASSERT_EQUAL_UINT32(0U, targetWrites());
}

// [test->fw~mc_008~1] a tap on a disabled drive enables the selected method with
// the dial at zero; dial travel then writes the speed target, and the next tap stops
static void test_a_tap_starts_the_selected_method_and_the_next_stops(void)
{
    tap();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_FOC_VELOCITY, mock_app_motorControl_lastMode);
    TEST_ASSERT_EQUAL_UINT32(0U, targetWrites());

    setMotor(APP_MOTORCONTROL_MODE_FOC_VELOCITY, APP_MOTORCONTROL_STATE_ENABLED, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    setDial_deg(120.0f);                                     // 90 degrees of travel: half of full scale
    passes(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setVelocityCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f * TEST_MAX_VEL, mock_app_motorControl_lastVelocity);

    tap();
    TEST_ASSERT_EQUAL_UINT32(2U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, mock_app_motorControl_lastMode);
    TEST_ASSERT_EQUAL_UINT32(1U, targetWrites());            // the stop writes no target
}

/* ---- dial targets across a method change ---- */

// A host method change re-seeds the dial's sent targets: no write until the dial
// itself moves, then a write in the new method's kind.
static void test_a_method_change_writes_no_stale_dial_target(void)
{
    tap();
    setMotor(APP_MOTORCONTROL_MODE_FOC_VELOCITY, APP_MOTORCONTROL_STATE_ENABLED, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    setDial_deg(120.0f);
    passes(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setVelocityCalls);

    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_DISABLED, APP_MOTORCONTROL_MODE_FOC_POSITION);
    passes(10U);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setPositionSetpointCalls);
    setDial_deg(130.0f);
    passes(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setPositionSetpointCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, DEG_TO_RAD(100.0f), mock_app_motorControl_lastPosition);

    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_DISABLED, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    passes(10U);
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setVelocityCalls);
    setDial_deg(140.0f);
    passes(1U);
    TEST_ASSERT_EQUAL_UINT32(2U, mock_app_motorControl_setVelocityCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, (110.0f / 180.0f) * TEST_MAX_VEL, mock_app_motorControl_lastVelocity);
}

// Dial movement inside the deadband writes nothing.
static void test_dial_jitter_inside_the_deadband_writes_nothing(void)
{
    tap();
    setMotor(APP_MOTORCONTROL_MODE_FOC_VELOCITY, APP_MOTORCONTROL_STATE_ENABLED, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    setDial_deg(30.4f);                                      // 0.04 rad/s of demand
    passes(5U);
    TEST_ASSERT_EQUAL_UINT32(0U, targetWrites());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_passes_write_nothing);
    RUN_TEST(test_the_first_tap_stops_a_host_started_drive);
    RUN_TEST(test_a_tap_stops_a_requested_drive_before_its_enable);
    RUN_TEST(test_a_tap_starts_the_selected_method_and_the_next_stops);
    RUN_TEST(test_a_method_change_writes_no_stale_dial_target);
    RUN_TEST(test_dial_jitter_inside_the_deadband_writes_nothing);
    return UNITY_END();
}
