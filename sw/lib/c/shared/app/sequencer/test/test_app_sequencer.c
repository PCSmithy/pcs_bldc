// Unit tests for app_sequencer (fw~ops_001, fw~ops_002): the module over the
// shared app_motorControl double. Each pass is one app_sequencer_run1ms; a
// write is seen as the double's setter recorders moving.

#include "app_sequencer.h"
#include "mock_app_motorControl.h"
#include "lib_utils.h"
#include "unity.h"

#define SEQ        (APP_SEQUENCER_CHANNEL_MAIN)
#define CAPACITY   (24U)   // the board's

static app_sequencer_setpoint_S storage[2U * CAPACITY];
static app_sequencer_channelConfig_S channelCfg[APP_SEQUENCER_CHANNEL_COUNT];
static app_sequencer_config_S config;

static void buildConfig(void)
{
    channelCfg[SEQ] = (app_sequencer_channelConfig_S){
        .motor = APP_MOTORCONTROL_CHANNEL_MAIN, .storage = storage, .capacity = CAPACITY };
    config = (app_sequencer_config_S){ .channels = channelCfg, .numChannels = APP_SEQUENCER_CHANNEL_COUNT };
}

static void setMotor(app_motorControl_state_E state, uint16_t faultLatches)
{
    app_motorControl_snapshot_S snap = { 0 };
    snap.state = state;
    snap.faultLatches = faultLatches;
    mock_app_motorControl_setSnapshot(&snap);
}

static void setMotorState(app_motorControl_state_E state)
{
    setMotor(state, 0U);
}

void setUp(void)
{
    mock_app_motorControl_reset();
    setMotorState(APP_MOTORCONTROL_STATE_ENABLED);
    buildConfig();
    TEST_ASSERT_TRUE(app_sequencer_init(&config));
}

void tearDown(void) {}

/* ---- helpers ---- */

static app_sequencer_setpoint_S velocityAt(uint32_t t_us, float32_t value, float32_t limit)
{
    return (app_sequencer_setpoint_S){ .t_us = t_us, .kind = APP_SEQUENCER_KIND_VELOCITY, .value = value, .rateLimit = limit };
}

static app_sequencer_setpoint_S torqueAt(uint32_t t_us, float32_t value, float32_t limit)
{
    return (app_sequencer_setpoint_S){ .t_us = t_us, .kind = APP_SEQUENCER_KIND_TORQUE, .value = value, .rateLimit = limit };
}

static app_sequencer_setpoint_S positionAt(uint32_t t_us, float32_t value, float32_t vLimit, float32_t aLimit, float32_t atPoint)
{
    return (app_sequencer_setpoint_S){ .t_us = t_us, .kind = APP_SEQUENCER_KIND_POSITION, .value = value,
                                       .rateLimit = vLimit, .accelLimit = aLimit, .velocityAtPoint = atPoint };
}

static void append(app_sequencer_setpoint_S sp)
{
    TEST_ASSERT_TRUE(app_sequencer_append(SEQ, &sp));
}

static void passes(uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++) { app_sequencer_run1ms(); }
}

static uint32_t writes(void)
{
    return mock_app_motorControl_setVelocityCalls + mock_app_motorControl_setTorqueCalls;
}

static app_sequencer_status_S status(void)
{
    app_sequencer_status_S s = { 0 };
    TEST_ASSERT_TRUE(app_sequencer_getStatus(SEQ, &s));
    return s;
}

// A committed three-speed sequence at 0, 250 and 500 ms, not yet started.
static void commitThreeSpeeds(void)
{
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(velocityAt(0U, 2.0f, 0.0f));
    append(velocityAt(250000U, 4.0f, 10.0f));
    append(velocityAt(500000U, 6.0f, 0.0f));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
}

/* ---- fw~ops_001: playback ---- */

// [test->fw~ops_001~1]
// Speed setpoints at 0, 250 000 and 500 000 us are written at passes 0, 250 and
// 500 from the start, the pass after the commit, each with its own rate limit.
static void test_setpoints_are_written_at_their_passes(void)
{
    commitThreeSpeeds();
    TEST_ASSERT_EQUAL_UINT32(0U, writes());
    TEST_ASSERT_FALSE(status().running);

    app_sequencer_run1ms();                                  // pass 0
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, mock_app_motorControl_lastVelocity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, mock_app_motorControl_lastVelocityLimit);
    TEST_ASSERT_TRUE(status().running);

    passes(249U);                                            // through pass 249
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    app_sequencer_run1ms();                                  // pass 250
    TEST_ASSERT_EQUAL_UINT32(2U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.0f, mock_app_motorControl_lastVelocity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, mock_app_motorControl_lastVelocityLimit);

    passes(249U);                                            // through pass 499
    TEST_ASSERT_EQUAL_UINT32(2U, writes());
    app_sequencer_run1ms();                                  // pass 500
    TEST_ASSERT_EQUAL_UINT32(3U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, mock_app_motorControl_lastVelocity);
    TEST_ASSERT_EQUAL_UINT32(501U, status().elapsed_ms);
}

// [test->fw~ops_001~1]
// Two setpoints at the same time are written in the same pass, in list order.
static void test_same_time_setpoints_write_in_list_order(void)
{
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(velocityAt(100000U, 1.0f, 0.0f));
    append(velocityAt(100000U, 2.0f, 0.0f));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    passes(100U);                                            // passes 0..99
    TEST_ASSERT_EQUAL_UINT32(0U, writes());
    app_sequencer_run1ms();                                  // pass 100
    TEST_ASSERT_EQUAL_UINT32(2U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, mock_app_motorControl_lastVelocity);
}

// [test->fw~ops_001~1]
// A position setpoint at 100 000 us reaches the position setter at pass 100 with
// its limits and its velocity at the point.
static void test_a_position_setpoint_writes_the_position_setpoint(void)
{
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(positionAt(100000U, 6.28f, 10.0f, 50.0f, 2.0f));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    passes(100U);                                            // passes 0..99
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setPositionSetpointCalls);
    app_sequencer_run1ms();                                  // pass 100
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setPositionSetpointCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.28f, mock_app_motorControl_lastPosition);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, mock_app_motorControl_lastPositionVelocityLimit);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 50.0f, mock_app_motorControl_lastPositionAccelLimit);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, mock_app_motorControl_lastPositionVelocityAtPoint);
}

// [test->fw~ops_001~1]
// A torque setpoint reaches the torque setter with its limit.
static void test_a_torque_setpoint_writes_the_torque_target(void)
{
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(torqueAt(0U, 0.3f, 0.5f));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setTorqueCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setVelocityCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.3f, mock_app_motorControl_lastTorque);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, mock_app_motorControl_lastTorqueLimit);
}

// [test->fw~ops_001~1]
// A stop at 300 000 us requests MODE_OFF at pass 300 from the start and
// writes no target; the sequence ends with it.
static void test_a_stop_setpoint_requests_mode_off_at_its_pass(void)
{
    const app_sequencer_setpoint_S stop = {
        .t_us = 300000U, .kind = APP_SEQUENCER_KIND_STOP, .value = 0.0f, .rateLimit = 0.0f };
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(velocityAt(0U, 2.0f, 0.0f));
    append(stop);
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    passes(300U);   // the start pass and 299 more: the stop is not yet due
    TEST_ASSERT_EQUAL_UINT32(0U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_TRUE(status().running);
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, mock_app_motorControl_setModeCalls);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, mock_app_motorControl_lastMode);
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FALSE(status().running);
}

// [test->fw~ops_001~1]
// The capacity of 24 is held and written in full, in either half of the
// storage; a 25th append is refused.
static void test_capacity_is_held_in_full(void)
{
    commitThreeSpeeds();
    passes(1U);                                              // the first half is running
    mock_app_motorControl_reset();
    setMotorState(APP_MOTORCONTROL_STATE_ENABLED);

    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    for (uint32_t i = 0U; i < CAPACITY; i++)
    {
        append(velocityAt(i * 1000U, (float32_t)i, 0.0f));
    }
    app_sequencer_setpoint_S extra = velocityAt(CAPACITY * 1000U, 99.0f, 0.0f);
    TEST_ASSERT_FALSE(app_sequencer_append(SEQ, &extra));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));

    passes(CAPACITY);                                        // passes 0..23
    TEST_ASSERT_EQUAL_UINT32(CAPACITY, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, (float32_t)(CAPACITY - 1U), mock_app_motorControl_lastVelocity);
}

// [test->fw~ops_001~1]
// After the last setpoint no further write occurs, running clears, and the
// elapsed time holds.
static void test_no_write_after_the_last_setpoint(void)
{
    commitThreeSpeeds();
    passes(501U);
    TEST_ASSERT_EQUAL_UINT32(3U, writes());
    TEST_ASSERT_FALSE(status().running);
    passes(1000U);
    TEST_ASSERT_EQUAL_UINT32(3U, writes());
    TEST_ASSERT_EQUAL_UINT32(501U, status().elapsed_ms);
}

/* ---- fw~ops_002: replacement and abort ---- */

// [test->fw~ops_002~1]
// A commit between two setpoints leaves the second unwritten; the new
// sequence starts on the pass after.
static void test_a_commit_replaces_the_running_sequence(void)
{
    commitThreeSpeeds();
    passes(100U);                                            // the 0 ms setpoint written
    TEST_ASSERT_EQUAL_UINT32(1U, writes());

    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(velocityAt(0U, 9.0f, 0.0f));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    app_sequencer_run1ms();                                  // the new pass 0
    TEST_ASSERT_EQUAL_UINT32(2U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 9.0f, mock_app_motorControl_lastVelocity);
    TEST_ASSERT_EQUAL_UINT32(1U, status().elapsed_ms);               // one pass run

    passes(1000U);                                           // the old 250 and 500 ms setpoints never come
    TEST_ASSERT_EQUAL_UINT32(2U, writes());
}

// [test->fw~ops_002~1]
// An empty commit ends the running sequence; no further setpoint is written.
static void test_an_empty_commit_stops(void)
{
    commitThreeSpeeds();
    passes(100U);
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    passes(1000U);
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FALSE(status().running);
}

// [test->fw~ops_002~1]
// A fault latched between two setpoints leaves the second unwritten, through
// and after the fault's clearing.
static void test_a_fault_drops_the_rest(void)
{
    commitThreeSpeeds();
    passes(100U);
    setMotor(APP_MOTORCONTROL_STATE_FAULTED, 1U);
    passes(300U);                                            // past the 250 ms setpoint
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FALSE(status().running);
    setMotor(APP_MOTORCONTROL_STATE_ENABLED, 1U);
    passes(1000U);                                           // past the 500 ms one
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
}

// [test->fw~ops_002~1]
// A fault that latches and clears between two passes shows only as a latch
// count; it drops the rest the same.
static void test_a_latch_cleared_between_passes_drops_the_rest(void)
{
    commitThreeSpeeds();
    passes(100U);
    setMotor(APP_MOTORCONTROL_STATE_ENABLED, 1U);
    passes(1000U);
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FALSE(status().running);
}

// [test->fw~ops_002~1]
// A fault latched on the start pass writes nothing.
static void test_a_fault_on_the_start_pass_writes_nothing(void)
{
    commitThreeSpeeds();
    setMotor(APP_MOTORCONTROL_STATE_FAULTED, 1U);
    passes(600U);
    TEST_ASSERT_EQUAL_UINT32(0U, writes());
    TEST_ASSERT_FALSE(status().running);
}

/* ---- staging and init ---- */

// begin is refused while a commit awaits its pass, and so is append; after
// the pass the accepted sequence plays and staging opens again.
static void test_begin_is_refused_while_a_commit_awaits_its_pass(void)
{
    commitThreeSpeeds();
    TEST_ASSERT_FALSE(app_sequencer_begin(SEQ));
    app_sequencer_setpoint_S sp = velocityAt(0U, 7.0f, 0.0f);
    TEST_ASSERT_FALSE(app_sequencer_append(SEQ, &sp));
    app_sequencer_run1ms();                                  // pass 0 of the accepted sequence
    TEST_ASSERT_EQUAL_UINT32(1U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, mock_app_motorControl_lastVelocity);

    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    append(sp);
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    app_sequencer_run1ms();
    TEST_ASSERT_EQUAL_UINT32(2U, writes());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.0f, mock_app_motorControl_lastVelocity);
}

// Without a begin, append and commit are refused: nothing of an earlier
// sequence left in the storage is replayed.
static void test_append_and_commit_need_a_begin(void)
{
    commitThreeSpeeds();
    passes(501U);                                            // played out
    app_sequencer_setpoint_S sp = velocityAt(0U, 7.0f, 0.0f);
    TEST_ASSERT_FALSE(app_sequencer_append(SEQ, &sp));
    TEST_ASSERT_FALSE(app_sequencer_commit(SEQ));
    passes(600U);
    TEST_ASSERT_EQUAL_UINT32(3U, writes());

    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));                // an abandoned staging
    append(sp);
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));                // starts over, empty
    TEST_ASSERT_TRUE(app_sequencer_commit(SEQ));
    passes(600U);
    TEST_ASSERT_EQUAL_UINT32(3U, writes());
}

static void test_append_refuses_an_unknown_kind(void)
{
    TEST_ASSERT_TRUE(app_sequencer_begin(SEQ));
    app_sequencer_setpoint_S sp = velocityAt(0U, 1.0f, 0.0f);
    sp.kind = APP_SEQUENCER_KIND_COUNT;
    TEST_ASSERT_FALSE(app_sequencer_append(SEQ, &sp));
}

static void test_init_rejects_a_degenerate_config(void)
{
    buildConfig();
    channelCfg[SEQ].storage = NULL;
    TEST_ASSERT_FALSE(app_sequencer_init(&config));
    buildConfig();
    channelCfg[SEQ].capacity = 0U;
    TEST_ASSERT_FALSE(app_sequencer_init(&config));
    buildConfig();
    channelCfg[SEQ].motor = APP_MOTORCONTROL_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(app_sequencer_init(&config));
    buildConfig();
    config.numChannels = APP_SEQUENCER_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(app_sequencer_init(&config));
    buildConfig();
    config.channels = NULL;
    TEST_ASSERT_FALSE(app_sequencer_init(&config));
    TEST_ASSERT_FALSE(app_sequencer_init(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_setpoints_are_written_at_their_passes);
    RUN_TEST(test_same_time_setpoints_write_in_list_order);
    RUN_TEST(test_a_torque_setpoint_writes_the_torque_target);
    RUN_TEST(test_a_position_setpoint_writes_the_position_setpoint);
    RUN_TEST(test_a_stop_setpoint_requests_mode_off_at_its_pass);
    RUN_TEST(test_capacity_is_held_in_full);
    RUN_TEST(test_no_write_after_the_last_setpoint);
    RUN_TEST(test_a_commit_replaces_the_running_sequence);
    RUN_TEST(test_an_empty_commit_stops);
    RUN_TEST(test_a_fault_drops_the_rest);
    RUN_TEST(test_a_latch_cleared_between_passes_drops_the_rest);
    RUN_TEST(test_a_fault_on_the_start_pass_writes_nothing);
    RUN_TEST(test_begin_is_refused_while_a_commit_awaits_its_pass);
    RUN_TEST(test_append_and_commit_need_a_begin);
    RUN_TEST(test_append_refuses_an_unknown_kind);
    RUN_TEST(test_init_rejects_a_degenerate_config);
    return UNITY_END();
}
