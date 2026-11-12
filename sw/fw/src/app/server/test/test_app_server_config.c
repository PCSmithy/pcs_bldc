#include "app_server.h"
#include "app_sequencer.h"
#include "mock_app_motorControl.h"   // shared shadow + controls
#include "mock_HW_ADC.h"
#include "lib_utils.h"
#include "unity.h"
#include <math.h>
#include <string.h>

#include "pb_encode.h"
#include "pb_decode.h"

extern const app_sequencer_config_S app_sequencer_config;   // the board's table

// The mock's ratings: K_t 0.5 Nm/A at 1 A, 20 rad/s.
#define TEST_KT          (0.5f)
#define TEST_I_MAX       (1.0f)
#define TEST_MAX_TORQUE  (TEST_KT * TEST_I_MAX)
#define TEST_MAX_VEL     (20.0f)
#define TEST_MAX_POS     (62.83f)
#define SEQ              (APP_SEQUENCER_CHANNEL_MAIN)

static void setTestRatings(void)
{
    const app_motorControl_ratings_S ratings = { .torqueConstant_NmPerA = TEST_KT, .maxCurrent_A = TEST_I_MAX,
                                                 .maxTorque_Nm = TEST_MAX_TORQUE, .maxVelocity_radPerSec = TEST_MAX_VEL,
                                                 .maxPosition_rad = TEST_MAX_POS };
    mock_app_motorControl_setRatings(&ratings);
}

void setUp(void)
{
    mock_HW_ADC_reset();
    mock_app_motorControl_reset();
    app_motorControl_snapshot_S snap = { 0 };
    mock_app_motorControl_setSnapshot(&snap);
    setTestRatings();
    TEST_ASSERT_TRUE(app_sequencer_init(&app_sequencer_config));
}

void tearDown(void) {}

static void setMotor(app_motorControl_mode_E mode, app_motorControl_state_E state);

/* ---- buildTelemetry: the board's snapshot + VBUS mapping ---- */

// [test->fw~obs_status_001~1]
static void test_telemetry_maps_snapshot_and_bus_measurements(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.mode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP;
    snap.state = APP_MOTORCONTROL_STATE_ENABLED;
    snap.velocityMeasured_radPerSec = 12.5f;
    snap.velocitySetpoint_radPerSec = 10.0f;
    snap.torqueTarget_Nm = 0.25f;
    snap.positionTarget_rad = 3.5f;
    snap.rotorPosition_rad = 3.25f;
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
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, telemetry.torque_target_nm);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.5f, telemetry.position_target_rad);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.25f, telemetry.rotor_position_rad);
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

// [test->fw~obs_status_001~1]
// Every method mode maps to its wire value.
static void test_telemetry_reports_each_method_mode(void)
{
    static const struct { app_motorControl_mode_E mode; board_Mode wire; } cases[] = {
        { APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, board_Mode_MODE_SIX_STEP_TRAP },
        { APP_MOTORCONTROL_MODE_SINUSOIDAL,    board_Mode_MODE_SINUSOIDAL },
        { APP_MOTORCONTROL_MODE_FOC_TORQUE,    board_Mode_MODE_FOC_TORQUE },
        { APP_MOTORCONTROL_MODE_FOC_VELOCITY,  board_Mode_MODE_FOC_VELOCITY },
        { APP_MOTORCONTROL_MODE_FOC_POSITION,  board_Mode_MODE_FOC_POSITION },
    };
    for (uint32_t i = 0U; i < COUNTOF(cases); i++)
    {
        setMotor(cases[i].mode, APP_MOTORCONTROL_STATE_ENABLED);
        board_Telemetry telemetry = board_Telemetry_init_zero;
        TEST_ASSERT_TRUE(app_server_config.buildTelemetry(&telemetry));
        TEST_ASSERT_EQUAL(cases[i].wire, telemetry.mode);
    }
}

/* ---- board command handling ---- */

static shared_Response sendCommand(const board_Request * const request)
{
    board_Reply reply = board_Reply_init_zero;
    shared_Response response = shared_Response_init_zero;
    TEST_ASSERT_FALSE(app_server_config.handleRequest(request, &reply, &response));
    return response;
}

/* ---- fw~conn_server_007: Command through the decode its arm hook arms ---- */

typedef struct
{
    const board_Setpoint * items;
    uint32_t count;
} setpointList_S;

static setpointList_S sentSetpoints;

static bool encodeSetpoints(pb_ostream_t * stream, const pb_field_t * field, void * const * arg)
{
    const setpointList_S * const list = (const setpointList_S *) *arg;
    bool ok = true;
    for (uint32_t i = 0U; (ok) && (i < list->count); i++)
    {
        ok = (pb_encode_tag_for_field(stream, field)) &&
             (pb_encode_submessage(stream, board_Setpoint_fields, &list->items[i]));
    }
    return ok;
}

static board_Setpoint speedAt(uint32_t t_us, float32_t value, float32_t limit)
{
    board_Setpoint sp = board_Setpoint_init_zero;
    sp.t_us = t_us;
    sp.which_target = board_Setpoint_velocity_radps_tag;
    sp.target.velocity_radps = value;
    sp.rate_limit = limit;
    return sp;
}

static board_Setpoint torqueAt(uint32_t t_us, float32_t value, float32_t limit)
{
    board_Setpoint sp = board_Setpoint_init_zero;
    sp.t_us = t_us;
    sp.which_target = board_Setpoint_torque_nm_tag;
    sp.target.torque_nm = value;
    sp.rate_limit = limit;
    return sp;
}

static board_Setpoint positionAt(uint32_t t_us, float32_t value, float32_t vLimit, float32_t aLimit, float32_t atPoint)
{
    board_Setpoint sp = board_Setpoint_init_zero;
    sp.t_us = t_us;
    sp.which_target = board_Setpoint_position_rad_tag;
    sp.target.position_rad = value;
    sp.rate_limit = vLimit;
    sp.accel_limit_radps2 = aLimit;
    sp.velocity_at_point_radps = atPoint;
    return sp;
}

// Encode a Command as the host does and decode it as the server does: the
// Request-arm hook arms the setpoint stream, then the handler answers.
static shared_Response sendRun(board_Mode mode, const board_Setpoint * const setpoints, uint32_t count)
{
    sentSetpoints = (setpointList_S){ .items = setpoints, .count = count };
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_run_tag;
    request.command.run.mode = mode;
    request.command.run.setpoints.funcs.encode = encodeSetpoints;
    request.command.run.setpoints.arg = &sentSetpoints;
    uint8_t buffer[640];
    pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(pb_encode(&ostream, board_Request_fields, &request));

    board_Request decoded = board_Request_init_zero;
    decoded.cb_command.funcs.decode = app_server_config.prepareCommand;
    pb_istream_t istream = pb_istream_from_buffer(buffer, ostream.bytes_written);
    TEST_ASSERT_TRUE(pb_decode(&istream, board_Request_fields, &decoded));
    return sendCommand(&decoded);
}

static void passes(uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++) { app_sequencer_run1ms(); }
}

static uint32_t writes(void)
{
    return mock_app_motorControl_setVelocityCalls + mock_app_motorControl_setTorqueCalls;
}

static void setMotor(app_motorControl_mode_E mode, app_motorControl_state_E state)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.mode = mode;
    snap.state = state;
    mock_app_motorControl_setSnapshot(&snap);
}

// A running sequence a rejection must leave alone: MODE_OFF is accepted in
// every state and its setpoints play regardless; the 3 ms one is the witness.
static void startWitnessSequence(void)
{
    const board_Setpoint list[2] = { speedAt(0U, 1.0f, 0.0f), speedAt(3000U, 9.0f, 0.0f) };
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_OFF, list, 2U).accepted);
    app_sequencer_run1ms();                                  // started: the 0 ms setpoint written
}

// A rejection: no acceptance, a cause, no mode request.
static void assertVerdictRejected(shared_Response response, uint32_t modeCallsBefore)
{
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_TRUE(strlen(response.cause) > 0U);
    TEST_ASSERT_EQUAL_UINT32(modeCallsBefore, mock_app_motorControl_setModeCalls);
}

// Sent over a running sequence, a rejected Command leaves it to play on to
// its witness.
static void assertRejected(board_Mode mode, const board_Setpoint * const setpoints, uint32_t count)
{
    startWitnessSequence();
    const uint32_t modeCalls = mock_app_motorControl_setModeCalls;
    const uint32_t before = writes();
    assertVerdictRejected(sendRun(mode, setpoints, count), modeCalls);
    passes(5U);
    TEST_ASSERT_EQUAL_UINT32(before + 1U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 9.0f, mock_app_motorControl_lastVelocity);
}

// [test->fw~conn_server_007~1]
// Disabled and unfaulted, a method with one speed setpoint at time zero is
// accepted: the enable is requested with that method, and the setpoint
// reaches the speed target on the pass after.
static void test_command_is_accepted_and_its_sequence_plays(void)
{
    const board_Setpoint sp = speedAt(0U, 5.0f, 10.0f);
    const shared_Response response = sendRun(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);
    TEST_ASSERT_TRUE(response.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, strlen(response.cause));
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_FOC_VELOCITY, mock_app_motorControl_lastMode);
    TEST_ASSERT_EQUAL_UINT32(0U, writes());
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setVelocityCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 5.0f, mock_app_motorControl_lastVelocity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, mock_app_motorControl_lastVelocityLimit);
}

// [test->fw~conn_server_007~1]
// A torque setpoint reaches the torque target.
static void test_a_torque_setpoint_reaches_the_torque_target(void)
{
    const board_Setpoint sp = torqueAt(0U, 0.3f, 0.0f);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_TORQUE, &sp, 1U).accepted);
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setTorqueCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.3f, mock_app_motorControl_lastTorque);
}

// [test->fw~conn_server_007~1]
// The position method with one position setpoint at time zero within its bounds
// is accepted, and the setpoint reaches the position setter with its four fields.
static void test_a_position_setpoint_reaches_the_position_setpoint(void)
{
    const board_Setpoint sp = positionAt(0U, 6.0f, 10.0f, 50.0f, 2.0f);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_POSITION, &sp, 1U).accepted);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_FOC_POSITION, mock_app_motorControl_lastMode);
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setPositionSetpointCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, mock_app_motorControl_lastPosition);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, mock_app_motorControl_lastPositionVelocityLimit);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 50.0f, mock_app_motorControl_lastPositionAccelLimit);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, mock_app_motorControl_lastPositionVelocityAtPoint);
}

// [test->fw~conn_server_007~1]
// The position rows of the rejection table: past the maximum position, a velocity
// limit past the maximum velocity, a velocity at the point past its limit (the
// maximum velocity for a limit of zero), a negative or non-finite acceleration.
// At the bounds, with a zero limit, each is accepted.
static void test_each_position_rejection_condition_rejects(void)
{
    board_Setpoint sp = positionAt(0U, TEST_MAX_POS + 0.1f, 10.0f, 50.0f, 0.0f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // past the maximum position
    sp = positionAt(0U, -(TEST_MAX_POS + 0.1f), 10.0f, 50.0f, 0.0f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);
    sp = positionAt(0U, 1.0f, TEST_MAX_VEL + 0.1f, 50.0f, 0.0f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // velocity limit past the maximum velocity
    sp = positionAt(0U, 1.0f, 10.0f, 50.0f, 10.1f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // velocity at the point past its limit
    sp = positionAt(0U, 1.0f, 10.0f, 50.0f, -10.1f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);
    sp = positionAt(0U, 1.0f, 0.0f, 50.0f, TEST_MAX_VEL + 0.1f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // past the maximum velocity at a zero limit
    sp = positionAt(0U, 1.0f, 10.0f, -1.0f, 0.0f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // acceleration negative
    sp = positionAt(0U, 1.0f, 10.0f, NAN, 0.0f);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // acceleration not finite
    sp = positionAt(0U, 1.0f, 10.0f, 50.0f, INFINITY);
    assertRejected(board_Mode_MODE_FOC_POSITION, &sp, 1U);                   // velocity at the point not finite

    const board_Setpoint atBounds[3] = { positionAt(0U, TEST_MAX_POS, TEST_MAX_VEL, 0.0f, TEST_MAX_VEL),
                                         positionAt(0U, -TEST_MAX_POS, 10.0f, 50.0f, 10.0f),
                                         positionAt(0U, 1.0f, 0.0f, 0.0f, TEST_MAX_VEL) };
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_POSITION, atBounds, 3U).accepted);
}

// [test->fw~conn_server_007~1]
// MODE_OFF with an empty list is accepted in every state and requests the
// disable.
static void test_stop_is_accepted_in_every_state(void)
{
    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_DISABLED);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_OFF, NULL, 0U).accepted);
    app_sequencer_run1ms();
    setMotor(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, APP_MOTORCONTROL_STATE_ENABLED);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_OFF, NULL, 0U).accepted);
    app_sequencer_run1ms();
    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_FAULTED);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_OFF, NULL, 0U).accepted);
    TEST_ASSERT_EQUAL_UINT32(3U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, mock_app_motorControl_lastMode);
}

// [test->fw~conn_server_007~1]
// Any accepted Command replaces the running sequence from its first setpoint,
// a method with none included: the witness never lands.
static void test_an_accepted_command_replaces_the_running_sequence(void)
{
    startWitnessSequence();
    const uint32_t before = writes();
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, NULL, 0U).accepted);
    passes(5U);
    TEST_ASSERT_EQUAL_UINT32(before, writes());
}

// [test->fw~conn_server_007~1]
// A list at the player's capacity is accepted and plays in full; one more is
// rejected.
static void test_capacity_is_accepted_and_one_more_rejected(void)
{
    board_Setpoint list[APP_SEQUENCER_CAPACITY + 1U];
    for (uint32_t i = 0U; i < COUNTOF(list); i++)
    {
        list[i] = speedAt(i * 1000U, 0.5f * (float32_t)i, 0.0f);
    }
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, list, APP_SEQUENCER_CAPACITY).accepted);
    passes(APP_SEQUENCER_CAPACITY);
    TEST_ASSERT_EQUAL_UINT32(APP_SEQUENCER_CAPACITY, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f * (float32_t)(APP_SEQUENCER_CAPACITY - 1U), mock_app_motorControl_lastVelocity);

    mock_app_motorControl_reset();
    setTestRatings();
    assertRejected(board_Mode_MODE_FOC_VELOCITY, list, APP_SEQUENCER_CAPACITY + 1U);
}

// [test->fw~conn_server_007~1]
// Each condition of the rejection table rejects the command, the requested
// state and the running sequence unchanged.
static void test_each_rejection_condition_rejects(void)
{
    board_Setpoint sp = speedAt(0U, 5.0f, 0.0f);

    assertRejected((board_Mode) 99, &sp, 1U);                                 // no such mode

    setMotor(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, APP_MOTORCONTROL_STATE_ENABLED);
    assertRejected(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);                   // another method while enabled

    // Faulted, the sequencer itself drops a running sequence (fw~ops_002), so
    // this one checks the verdict alone.
    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_FAULTED);
    const uint32_t modeCalls = mock_app_motorControl_setModeCalls;
    assertVerdictRejected(sendRun(board_Mode_MODE_FOC_VELOCITY, &sp, 1U), modeCalls);   // a method while faulted
    setMotor(APP_MOTORCONTROL_MODE_OFF, APP_MOTORCONTROL_STATE_DISABLED);

    sp = speedAt(0U, NAN, 0.0f);
    assertRejected(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);                   // value not finite
    sp = speedAt(0U, 5.0f, INFINITY);
    assertRejected(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);                   // limit not finite
    sp = speedAt(0U, 5.0f, -1.0f);
    assertRejected(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);                   // limit negative
    sp = speedAt(0U, -(TEST_MAX_VEL + 0.1f), 0.0f);
    assertRejected(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);                   // past the maximum velocity
    sp = torqueAt(0U, TEST_MAX_TORQUE + 0.01f, 0.0f);
    assertRejected(board_Mode_MODE_FOC_TORQUE, &sp, 1U);                     // past K_t I_max
    sp = (board_Setpoint)board_Setpoint_init_zero;
    assertRejected(board_Mode_MODE_FOC_VELOCITY, &sp, 1U);                   // no target

    const board_Setpoint disordered[2] = { speedAt(250000U, 1.0f, 0.0f), speedAt(100000U, 2.0f, 0.0f) };
    assertRejected(board_Mode_MODE_FOC_VELOCITY, disordered, 2U);            // times out of order

    // The bounds are inclusive.
    const board_Setpoint atBounds[2] = { speedAt(0U, TEST_MAX_VEL, 0.0f), speedAt(0U, -TEST_MAX_VEL, 0.0f) };
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, atBounds, 2U).accepted);
}

// [test->fw~conn_server_007~1]
// Re-requesting the active method while enabled is accepted: a second
// Command under the running method mid-run plays.
static void test_the_active_method_is_accepted_while_enabled(void)
{
    setMotor(APP_MOTORCONTROL_MODE_FOC_VELOCITY, APP_MOTORCONTROL_STATE_ENABLED);
    const board_Setpoint sp = speedAt(0U, 3.0f, 0.0f);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, &sp, 1U).accepted);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_FOC_VELOCITY, mock_app_motorControl_lastMode);
    app_sequencer_run1ms();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, mock_app_motorControl_lastVelocity);
}

// [test->fw~conn_server_007~1]
// A second Command before the pass that starts the first is rejected as busy
// and leaves the first to play; after the pass it is accepted.
static void test_a_command_within_the_start_pass_is_rejected_busy(void)
{
    const board_Setpoint first = speedAt(0U, 1.0f, 0.0f);
    const board_Setpoint second = speedAt(0U, 2.0f, 0.0f);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, &first, 1U).accepted);
    const shared_Response response = sendRun(board_Mode_MODE_FOC_VELOCITY, &second, 1U);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(response.cause, "busy"));
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, mock_app_motorControl_lastVelocity);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, &second, 1U).accepted);
    app_sequencer_run1ms();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, mock_app_motorControl_lastVelocity);
}

// [test->fw~conn_server_007~1]
// A Command whose decode fails after setpoints streamed commits nothing; the
// next Command plays only its own list.
static void test_a_truncated_command_leaves_nothing_behind(void)
{
    const board_Setpoint list[2] = { speedAt(0U, 1.0f, 0.0f), speedAt(1000U, 2.0f, 0.0f) };
    sentSetpoints = (setpointList_S){ .items = list, .count = 2U };
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_run_tag;
    request.command.run.mode = board_Mode_MODE_FOC_VELOCITY;
    request.command.run.setpoints.funcs.encode = encodeSetpoints;
    request.command.run.setpoints.arg = &sentSetpoints;
    uint8_t buffer[64];
    pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(pb_encode(&ostream, board_Request_fields, &request));

    board_Request decoded = board_Request_init_zero;
    decoded.cb_command.funcs.decode = app_server_config.prepareCommand;
    pb_istream_t istream = pb_istream_from_buffer(buffer, ostream.bytes_written - 3U);   // the last setpoint cut short
    TEST_ASSERT_FALSE(pb_decode(&istream, board_Request_fields, &decoded));
    passes(5U);
    TEST_ASSERT_EQUAL_UINT32(0U, writes());

    const board_Setpoint own = speedAt(0U, 7.0f, 0.0f);
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, &own, 1U).accepted);
    passes(5U);
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.0f, mock_app_motorControl_lastVelocity);
}

// A run arm handed to the handler without its decode (no staging) is
// rejected rather than accepted with nothing to play.
static void test_an_unstaged_run_is_rejected(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_run_tag;
    request.command.run = (board_Command)board_Command_init_zero;
    request.command.run.mode = board_Mode_MODE_FOC_VELOCITY;
    const shared_Response response = sendCommand(&request);
    TEST_ASSERT_FALSE(response.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setModeCalls);
}

/* ---- fw~conn_server_008: the motor rating report ---- */

// [test->fw~conn_server_008~1]
static void test_get_motor_info_reports_the_ratings(void)
{
    board_Request request = board_Request_init_zero;
    request.which_command = board_Request_get_motor_info_tag;
    board_Reply reply = board_Reply_init_zero;
    shared_Response response = shared_Response_init_zero;
    TEST_ASSERT_TRUE(app_server_config.handleRequest(&request, &reply, &response));
    TEST_ASSERT_EQUAL(board_Reply_motor_info_tag, reply.which_result);
    TEST_ASSERT_FLOAT_WITHIN(0.001f * TEST_KT, TEST_KT, reply.result.motor_info.kt_nm_per_a);
    TEST_ASSERT_FLOAT_WITHIN(0.001f * TEST_I_MAX, TEST_I_MAX, reply.result.motor_info.i_max_a);
    TEST_ASSERT_FLOAT_WITHIN(0.001f * TEST_MAX_TORQUE, TEST_MAX_TORQUE, reply.result.motor_info.max_torque_nm);
    TEST_ASSERT_FLOAT_WITHIN(0.001f * TEST_MAX_VEL, TEST_MAX_VEL, reply.result.motor_info.max_velocity_radps);
    TEST_ASSERT_FLOAT_WITHIN(0.001f * TEST_MAX_POS, TEST_MAX_POS, reply.result.motor_info.max_position_rad);
}

// [test->fw~obs_status_001~1]
// Telemetry carries the sequence player's state: running with its elapsed
// time once a Command plays.
static void test_telemetry_reports_the_sequence(void)
{
    board_Telemetry telemetry = board_Telemetry_init_zero;
    TEST_ASSERT_TRUE(app_server_config.buildTelemetry(&telemetry));
    TEST_ASSERT_FALSE(telemetry.sequence_running);

    const board_Setpoint list[2] = { speedAt(0U, 1.0f, 0.0f), speedAt(500000U, 2.0f, 0.0f) };
    TEST_ASSERT_TRUE(sendRun(board_Mode_MODE_FOC_VELOCITY, list, 2U).accepted);
    passes(3U);
    TEST_ASSERT_TRUE(app_server_config.buildTelemetry(&telemetry));
    TEST_ASSERT_TRUE(telemetry.sequence_running);
    TEST_ASSERT_EQUAL_UINT32(3U, telemetry.sequence_elapsed_ms);
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
    RUN_TEST(test_telemetry_reports_each_method_mode);

    RUN_TEST(test_clear_fault_accepted_with_no_fault);
    RUN_TEST(test_empty_command_rejected);
    RUN_TEST(test_command_is_accepted_and_its_sequence_plays);
    RUN_TEST(test_a_torque_setpoint_reaches_the_torque_target);
    RUN_TEST(test_a_position_setpoint_reaches_the_position_setpoint);
    RUN_TEST(test_each_position_rejection_condition_rejects);
    RUN_TEST(test_stop_is_accepted_in_every_state);
    RUN_TEST(test_an_accepted_command_replaces_the_running_sequence);
    RUN_TEST(test_capacity_is_accepted_and_one_more_rejected);
    RUN_TEST(test_each_rejection_condition_rejects);
    RUN_TEST(test_the_active_method_is_accepted_while_enabled);
    RUN_TEST(test_a_command_within_the_start_pass_is_rejected_busy);
    RUN_TEST(test_a_truncated_command_leaves_nothing_behind);
    RUN_TEST(test_an_unstaged_run_is_rejected);
    RUN_TEST(test_get_motor_info_reports_the_ratings);
    RUN_TEST(test_telemetry_reports_the_sequence);

    return UNITY_END();
}
