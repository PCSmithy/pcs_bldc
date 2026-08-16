#include "app_server.h"
#include "app_motorControl.h"   // test-local shadow + mock controls
#include "mock_HW_ADC.h"
#include "unity.h"
#include <string.h>

void setUp(void)
{
    mock_HW_ADC_reset();
    app_motorControl_snapshot_S snap = { 0 };
    mock_app_motorControl_setSnapshot(&snap);
}

void tearDown(void) {}

/* ---- buildTelemetry: the board's snapshot + VBUS mapping ---- */

// [test->fw~obs_status_001~1]
static void test_telemetry_maps_snapshot_and_bus_measurements(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.mode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP;
    snap.state = APP_MOTORCONTROL_STATE_ENABLED;
    snap.velocityMeasured_radPerSec = 12.5f;
    snap.velocitySetpoint_radPerSec = 10.0f;
    mock_app_motorControl_setSnapshot(&snap);

    // VBUS pins: 1.8 V / 0.15 = 12 V bus; 0.3 V / 0.6 = 0.5 A.
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_1, 12U, 1.8f);
    mock_HW_ADC_setVolts(HW_ADC_CHANNEL_2, 11U, 0.3f);

    board_Telemetry telemetry = board_Telemetry_init_zero;
    TEST_ASSERT_TRUE(app_server_config.buildTelemetry(&telemetry));
    TEST_ASSERT_EQUAL(board_Mode_MODE_SIX_STEP_TRAP, telemetry.mode);
    TEST_ASSERT_EQUAL(board_DriveState_DRIVE_STATE_ENABLED, telemetry.state);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.0f, telemetry.bus_voltage_v);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, telemetry.bus_current_a);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, telemetry.velocity_measured_radps);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, telemetry.velocity_setpoint_radps);
}

// [test->fw~obs_status_001~1]
static void test_telemetry_reports_faulted_state(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.state = APP_MOTORCONTROL_STATE_FAULTED;
    mock_app_motorControl_setSnapshot(&snap);

    board_Telemetry telemetry = board_Telemetry_init_zero;
    TEST_ASSERT_TRUE(app_server_config.buildTelemetry(&telemetry));
    TEST_ASSERT_EQUAL(board_DriveState_DRIVE_STATE_FAULTED, telemetry.state);
    TEST_ASSERT_EQUAL(board_Mode_MODE_OFF, telemetry.mode);
}

/* ---- handleRequest: commands await the control-service work ---- */

static void test_commands_rejected_until_implemented(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_velocity_tag;
    request.command.set_velocity.velocity_radps = 5.0f;

    shared_Response response = shared_Response_init_zero;
    app_server_config.handleRequest(&request, &response);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_TRUE(strlen(response.cause) > 0U);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_telemetry_maps_snapshot_and_bus_measurements);
    RUN_TEST(test_telemetry_reports_faulted_state);
    RUN_TEST(test_commands_rejected_until_implemented);
    return UNITY_END();
}
