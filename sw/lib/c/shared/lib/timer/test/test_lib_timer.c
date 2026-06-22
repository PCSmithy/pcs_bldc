#include "lib_timer.h"
#include "unity.h"

// ---------------------------------------------------------------------------
// Fake time source. lib_timer.c externs `lib_timer_config` and binds its time
// base to it at static init, so the test supplies the definition: a settable
// 32-bit "HW counter" the tests drive directly. No HW_TIM dependency.
// ---------------------------------------------------------------------------
static uint32_t fakeCounter_us;

static uint32_t test_getTime_us(void)
{
    return fakeCounter_us;
}

const lib_timer_config_S lib_timer_config =
{
    .getTime_us = test_getTime_us,
};

// Advance the fake HW counter (wraps at 2^32, like the real 32-bit TIM2).
static void advance(uint32_t deltaUs)
{
    fakeCounter_us += deltaUs;
}

// Park the counter at `t` and sync the module to it, so a test's later reads
// are clean deltas from this point. The module's accumulator has no reset, so
// every assertion below is delta-based and order-independent; `syncTo` returns
// the accumulated baseline for tests that need it.
static uint64_t syncTo(uint32_t t)
{
    fakeCounter_us = t;
    return lib_timer_getTime_us();   // update(): lastTime <- t; returns accumulator
}

void setUp(void) {}
void tearDown(void) {}

/* ----------------------------- time base ------------------------------ */

// [test->fw~hal_tim_003~1]
static void test_getTime_us_advances_by_delta(void)
{
    const uint64_t base = syncTo(1000U);
    advance(1234U);
    TEST_ASSERT_EQUAL_UINT64(base + 1234U, lib_timer_getTime_us());
}

static void test_getTime_us_accumulates_across_reads(void)
{
    const uint64_t base = syncTo(0U);
    advance(100U);
    TEST_ASSERT_EQUAL_UINT64(base + 100U, lib_timer_getTime_us());
    advance(250U);
    TEST_ASSERT_EQUAL_UINT64(base + 350U, lib_timer_getTime_us());
    advance(0U);
    TEST_ASSERT_EQUAL_UINT64(base + 350U, lib_timer_getTime_us());   // no advance -> no change
}

static void test_getTime_us_handles_counter_wrap(void)
{
    const uint64_t base = syncTo(0xFFFFFF00U);
    advance(0x200U);   // crosses the 2^32 boundary; true elapsed is 0x200
    TEST_ASSERT_EQUAL_UINT64(base + 0x200U, lib_timer_getTime_us());
}

static void test_getTime_us_wrap_exactly_at_max(void)
{
    const uint64_t base = syncTo(0xFFFFFFFFU);
    advance(1U);       // counter -> 0
    TEST_ASSERT_EQUAL_UINT64(base + 1U, lib_timer_getTime_us());
}

static void test_run_catches_intermediate_wrap(void)
{
    // A single read can only recover one wrap (delta is mod 2^32). Calling
    // lib_timer_run() between sub-2^32 advances is what lets total elapsed
    // exceed 2^32 -- that is the whole point of running it periodically.
    const uint64_t base = syncTo(0U);
    advance(3000000000U);   // 3e9 < 2^32
    lib_timer_run();
    advance(3000000000U);   // counter wraps; run() already banked the first 3e9
    lib_timer_run();
    TEST_ASSERT_EQUAL_UINT64(base + 6000000000ULL, lib_timer_getTime_us());
}

static void test_single_read_cannot_recover_double_wrap(void)
{
    // Same 6e9 of advance but with no intermediate run(): the lone read sees
    // only (6e9 mod 2^32). Documents the periodic-run contract.
    const uint64_t base = syncTo(0U);
    advance(3000000000U);
    advance(3000000000U);
    TEST_ASSERT_EQUAL_UINT64(base + (6000000000ULL - 0x100000000ULL),
                             lib_timer_getTime_us());
}

static void test_getTime_ms_matches_us_over_1000(void)
{
    syncTo(0U);
    advance(123456U);
    const uint64_t us = lib_timer_getTime_us();      // updates accumulator
    const uint32_t ms = lib_timer_getTime_ms();      // no further advance
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(us / 1000U), ms);
}

static void test_getTime_ms_advances_by_whole_ms(void)
{
    syncTo(0U);
    const uint32_t base = lib_timer_getTime_ms();
    advance(5000U);                                  // exactly 5 ms
    TEST_ASSERT_EQUAL_UINT32(base + 5U, lib_timer_getTime_ms());
}

/* --------------------------- channel: setup --------------------------- */

static void test_init_sets_inactive_and_fields(void)
{
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 500U);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_INACTIVE, ch.state);
    TEST_ASSERT_EQUAL(LIB_TIMER_PRECISION_US, ch.precision);
    TEST_ASSERT_EQUAL_UINT64(500U, ch.duration);
}

static void test_null_channel_is_safe(void)
{
    lib_timer_init(NULL, LIB_TIMER_PRECISION_US, 100U);
    lib_timer_startTimer(NULL);
    lib_timer_stopTimer(NULL);
    TEST_ASSERT_EQUAL_UINT64(0U, lib_timer_getElapsedTime(NULL));
    TEST_ASSERT_EQUAL_UINT64(0U, lib_timer_getRemainingTime(NULL));
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_UNINITIALIZED, lib_timer_updateTimerAndGetState(NULL));
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_UNINITIALIZED, lib_timer_runTimerWithEnable(NULL, true));
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_UNINITIALIZED, lib_timer_runTimerWithRestart(NULL, true));
}

static void test_startTimer_sets_running(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 500U);
    lib_timer_startTimer(&ch);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_RUNNING, ch.state);
}

static void test_startTimer_uninitialized_is_noop(void)
{
    lib_timer_channel_S ch = { 0 };   // state == UNINITIALIZED
    lib_timer_startTimer(&ch);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_UNINITIALIZED, ch.state);
}

static void test_stopTimer_sets_inactive(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 500U);
    lib_timer_startTimer(&ch);
    lib_timer_stopTimer(&ch);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_INACTIVE, ch.state);
}

/* ------------------------- channel: elapsed --------------------------- */

static void test_elapsed_us_tracks_advance(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    advance(300U);
    TEST_ASSERT_EQUAL_UINT64(300U, lib_timer_getElapsedTime(&ch));
}

static void test_elapsed_ms_tracks_advance(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_MS, 100U);
    lib_timer_startTimer(&ch);
    advance(3000U);   // 3 ms
    TEST_ASSERT_EQUAL_UINT64(3U, lib_timer_getElapsedTime(&ch));
}

static void test_elapsed_inactive_is_zero(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);   // INACTIVE, never started
    advance(500U);
    TEST_ASSERT_EQUAL_UINT64(0U, lib_timer_getElapsedTime(&ch));
}

/* ------------------------ channel: remaining -------------------------- */

static void test_remaining_counts_down(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    advance(300U);
    TEST_ASSERT_EQUAL_UINT64(700U, lib_timer_getRemainingTime(&ch));
}

static void test_remaining_clamps_at_zero(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    advance(1500U);   // past the duration
    TEST_ASSERT_EQUAL_UINT64(0U, lib_timer_getRemainingTime(&ch));
}

/* --------------------- channel: state transitions --------------------- */

static void test_update_expires_past_duration(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    advance(1001U);   // strictly greater than duration
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_EXPIRED, lib_timer_updateTimerAndGetState(&ch));
}

static void test_update_stays_running_within_duration(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    advance(1000U);   // equal to duration -> not yet expired
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_RUNNING, lib_timer_updateTimerAndGetState(&ch));
}

static void test_runWithEnable_true_starts_and_expires(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_RUNNING, lib_timer_runTimerWithEnable(&ch, true));
    advance(1001U);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_EXPIRED, lib_timer_runTimerWithEnable(&ch, true));
}

static void test_runWithEnable_false_stops(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_INACTIVE, lib_timer_runTimerWithEnable(&ch, false));
}

static void test_runWithRestart_rebaselines(void)
{
    syncTo(0U);
    lib_timer_channel_S ch = { 0 };
    lib_timer_init(&ch, LIB_TIMER_PRECISION_US, 1000U);
    lib_timer_startTimer(&ch);
    advance(900U);
    // Restart just before expiry: elapsed resets, so it stays RUNNING.
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_RUNNING, lib_timer_runTimerWithRestart(&ch, true));
    advance(900U);   // 1800 total since first start, but only 900 since restart
    TEST_ASSERT_EQUAL(LIB_TIMER_STATE_RUNNING, lib_timer_updateTimerAndGetState(&ch));
}

int main(void)
{
    UNITY_BEGIN();

    // time base
    RUN_TEST(test_getTime_us_advances_by_delta);
    RUN_TEST(test_getTime_us_accumulates_across_reads);
    RUN_TEST(test_getTime_us_handles_counter_wrap);
    RUN_TEST(test_getTime_us_wrap_exactly_at_max);
    RUN_TEST(test_run_catches_intermediate_wrap);
    RUN_TEST(test_single_read_cannot_recover_double_wrap);
    RUN_TEST(test_getTime_ms_matches_us_over_1000);
    RUN_TEST(test_getTime_ms_advances_by_whole_ms);

    // channel setup
    RUN_TEST(test_init_sets_inactive_and_fields);
    RUN_TEST(test_null_channel_is_safe);
    RUN_TEST(test_startTimer_sets_running);
    RUN_TEST(test_startTimer_uninitialized_is_noop);
    RUN_TEST(test_stopTimer_sets_inactive);

    // elapsed
    RUN_TEST(test_elapsed_us_tracks_advance);
    RUN_TEST(test_elapsed_ms_tracks_advance);
    RUN_TEST(test_elapsed_inactive_is_zero);

    // remaining
    RUN_TEST(test_remaining_counts_down);
    RUN_TEST(test_remaining_clamps_at_zero);

    // state transitions
    RUN_TEST(test_update_expires_past_duration);
    RUN_TEST(test_update_stays_running_within_duration);
    RUN_TEST(test_runWithEnable_true_starts_and_expires);
    RUN_TEST(test_runWithEnable_false_stops);
    RUN_TEST(test_runWithRestart_rebaselines);

    return UNITY_END();
}
