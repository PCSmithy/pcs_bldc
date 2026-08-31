#include "app_server.h"
#include "app_motorControl.h"   // test-local shadow + mock controls
#include "mock_HW_ADC.h"
#include "unity.h"
#include <math.h>
#include <string.h>

void setUp(void)
{
    mock_HW_ADC_reset();
    mock_app_motorControl_resetRecorders();
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

/* ---- fw~conn_server_002: board command handling ---- */

static shared_Response sendCommand(const board_Request * const request)
{
    shared_Response response = shared_Response_init_zero;
    app_server_config.handleRequest(request, &response);
    return response;
}

// [test->fw~conn_server_003~1]
static void test_set_velocity_in_range_accepted(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_velocity_tag;
    request.command.set_velocity.velocity_radps = -5.0f;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_TRUE(response.accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setVelocityCalls);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, mock_app_motorControl_lastVelocity);
}

// [test->fw~conn_server_003~1]
static void test_set_velocity_out_of_range_rejected(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_velocity_tag;
    request.command.set_velocity.velocity_radps = APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC + 1.0f;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_TRUE(strlen(response.cause) > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setVelocityCalls);
}

// [test->fw~conn_server_003~1]
static void test_set_velocity_nan_rejected(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_velocity_tag;
    request.command.set_velocity.velocity_radps = NAN;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setVelocityCalls);
}

// [test->fw~conn_server_002~1]
static void test_set_mode_accepted_and_applied(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_mode_tag;
    request.command.set_mode.mode = board_Mode_MODE_SIX_STEP_TRAP;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_TRUE(response.accepted);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, mock_app_motorControl_lastMode);
}

// [test->fw~conn_server_002~1]
static void test_set_mode_unknown_value_rejected(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_mode_tag;
    request.command.set_mode.mode = (board_Mode) 7;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_TRUE(strlen(response.cause) > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setModeCalls);
}

// [test->fw~conn_server_002~1]
// Method selection only while the drive is off: a different method mid-run is
// rejected; stopping (MODE_OFF) and re-requesting the active method pass.
static void test_set_mode_gated_while_drive_enabled(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.mode = APP_MOTORCONTROL_MODE_OFF;   // mock-only: enabled with a differing active method
    snap.state = APP_MOTORCONTROL_STATE_ENABLED;
    mock_app_motorControl_setSnapshot(&snap);

    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_mode_tag;
    request.command.set_mode.mode = board_Mode_MODE_SIX_STEP_TRAP;
    shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_TRUE(strlen(response.cause) > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setModeCalls);

    // Stopping is always accepted, enabled or not.
    request.command.set_mode.mode = board_Mode_MODE_OFF;
    response = sendCommand(&request);
    TEST_ASSERT_TRUE(response.accepted);

    // Re-requesting the active method while enabled is accepted.
    snap.mode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP;
    mock_app_motorControl_setSnapshot(&snap);
    request.command.set_mode.mode = board_Mode_MODE_SIX_STEP_TRAP;
    response = sendCommand(&request);
    TEST_ASSERT_TRUE(response.accepted);
}

// [test->fw~conn_server_002~1]
// Button-path parity: no method selection while a fault is latched; stopping
// stays accepted.
static void test_set_mode_method_rejected_while_faulted(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.state = APP_MOTORCONTROL_STATE_FAULTED;
    mock_app_motorControl_setSnapshot(&snap);

    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_set_mode_tag;
    request.command.set_mode.mode = board_Mode_MODE_SIX_STEP_TRAP;
    shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setModeCalls);

    request.command.set_mode.mode = board_Mode_MODE_OFF;
    response = sendCommand(&request);
    TEST_ASSERT_TRUE(response.accepted);
}

// [test->fw~conn_server_004~1]
static void test_clear_fault_accepted_with_no_fault(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_clear_fault_tag;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_TRUE(response.accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_clearFaultCalls);
}

static void test_empty_command_rejected(void)
{
    board_Request request = board_Request_init_zero;

    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_TRUE(strlen(response.cause) > 0U);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_telemetry_maps_snapshot_and_bus_measurements);
    RUN_TEST(test_telemetry_reports_faulted_state);

    RUN_TEST(test_set_velocity_in_range_accepted);
    RUN_TEST(test_set_velocity_out_of_range_rejected);
    RUN_TEST(test_set_velocity_nan_rejected);
    RUN_TEST(test_set_mode_accepted_and_applied);
    RUN_TEST(test_set_mode_unknown_value_rejected);
    RUN_TEST(test_set_mode_gated_while_drive_enabled);
    RUN_TEST(test_set_mode_method_rejected_while_faulted);
    RUN_TEST(test_clear_fault_accepted_with_no_fault);
    RUN_TEST(test_empty_command_rejected);

    return UNITY_END();
}
