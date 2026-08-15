#include "SIL_irq.h"
#include "unity.h"

// Call log for the fake vtable, so pass-through of every argument is provable.
typedef struct
{
    void *            lastContext;
    SIL_irq_handler_F lastHandler;
    uint32_t          lastRateOrDelay_us;
    uint8_t           lastPriority;
    int32_t           registerReturn;
    uint32_t          periodicCalls;
    uint32_t          oneShotCalls;

    int32_t  lastCancelHandle;
    uint32_t cancelCalls;

    int32_t  lastEnableHandle;
    bool     lastEnabled;
    uint32_t enableCalls;
} fakeHooksLog_S;

static fakeHooksLog_S fakeLog;
static uint32_t contextTag;   // any stable address to use as the context

// Two distinct handler addresses; never called by these tests.
static void handlerA(void)
{
}

static void handlerB(void)
{
}

static int32_t fakePeriodic(void * context, SIL_irq_handler_F handler,
                            uint32_t period_us, uint8_t priority)
{
    fakeLog.lastContext        = context;
    fakeLog.lastHandler        = handler;
    fakeLog.lastRateOrDelay_us = period_us;
    fakeLog.lastPriority       = priority;
    fakeLog.periodicCalls++;
    return fakeLog.registerReturn;
}

static int32_t fakeOneShot(void * context, SIL_irq_handler_F handler,
                           uint32_t delay_us, uint8_t priority)
{
    fakeLog.lastContext        = context;
    fakeLog.lastHandler        = handler;
    fakeLog.lastRateOrDelay_us = delay_us;
    fakeLog.lastPriority       = priority;
    fakeLog.oneShotCalls++;
    return fakeLog.registerReturn;
}

static void fakeCancel(void * context, int32_t handle)
{
    fakeLog.lastContext      = context;
    fakeLog.lastCancelHandle = handle;
    fakeLog.cancelCalls++;
}

static void fakeSetEnabled(void * context, int32_t handle, bool enabled)
{
    fakeLog.lastContext      = context;
    fakeLog.lastEnableHandle = handle;
    fakeLog.lastEnabled      = enabled;
    fakeLog.enableCalls++;
}

static void installFakeHooks(void)
{
    const SIL_irq_hooks_S hooks = {
        .context          = &contextTag,
        .registerPeriodic = fakePeriodic,
        .registerOneShot  = fakeOneShot,
        .cancel           = fakeCancel,
        .setEnabled       = fakeSetEnabled,
    };
    SIL_irq_setHooks(&hooks);
}

void setUp(void)
{
    SIL_irq_setHooks(NULL);
    fakeLog = (fakeHooksLog_S){ 0 };
}

void tearDown(void)
{
    SIL_irq_setHooks(NULL);
}

/* ---- no hooks installed: everything is a safe no-op ---- */

static void test_no_hooks_register_returns_invalid(void)
{
    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerPeriodic(handlerA, 50U, 0U));
    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerOneShot(handlerA, 2U, 0U));
}

static void test_no_hooks_cancel_and_enable_are_noops(void)
{
    SIL_irq_cancel(0);              // must not crash
    SIL_irq_setEnabled(0, true);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.cancelCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.enableCalls);
}

/* ---- hooks installed: arguments pass straight through ---- */

static void test_register_periodic_passes_through(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 4;

    TEST_ASSERT_EQUAL_INT32(4, SIL_irq_registerPeriodic(handlerA, 50U, 3U));
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.periodicCalls);
    TEST_ASSERT_EQUAL_PTR(&contextTag, fakeLog.lastContext);
    TEST_ASSERT_EQUAL_PTR(handlerA, fakeLog.lastHandler);
    TEST_ASSERT_EQUAL_UINT32(50U, fakeLog.lastRateOrDelay_us);
    TEST_ASSERT_EQUAL_UINT8(3U, fakeLog.lastPriority);
}

static void test_register_one_shot_passes_through(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 9;

    TEST_ASSERT_EQUAL_INT32(9, SIL_irq_registerOneShot(handlerB, 2U, 7U));
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.oneShotCalls);
    TEST_ASSERT_EQUAL_PTR(handlerB, fakeLog.lastHandler);
    TEST_ASSERT_EQUAL_UINT32(2U, fakeLog.lastRateOrDelay_us);
    TEST_ASSERT_EQUAL_UINT8(7U, fakeLog.lastPriority);
}

static void test_one_shot_accepts_zero_delay(void)
{
    // A zero delay is legal for a one-shot: it means "the next grid step".
    installFakeHooks();
    fakeLog.registerReturn = 1;
    TEST_ASSERT_EQUAL_INT32(1, SIL_irq_registerOneShot(handlerA, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.oneShotCalls);
}

static void test_register_rejects_null_handler_and_zero_period_locally(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 4;

    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerPeriodic(NULL, 50U, 0U));
    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerPeriodic(handlerA, 0U, 0U));
    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerOneShot(NULL, 2U, 0U));
    // None of the three reached the framework.
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.periodicCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.oneShotCalls);
}

static void test_cancel_and_set_enabled_pass_through(void)
{
    installFakeHooks();

    SIL_irq_cancel(6);
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.cancelCalls);
    TEST_ASSERT_EQUAL_INT32(6, fakeLog.lastCancelHandle);

    SIL_irq_setEnabled(6, false);
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.enableCalls);
    TEST_ASSERT_EQUAL_INT32(6, fakeLog.lastEnableHandle);
    TEST_ASSERT_FALSE(fakeLog.lastEnabled);

    SIL_irq_setEnabled(6, true);
    TEST_ASSERT_TRUE(fakeLog.lastEnabled);
}

static void test_cancel_and_enable_guard_invalid_handle(void)
{
    installFakeHooks();
    SIL_irq_cancel(SIL_IRQ_HANDLE_INVALID);
    SIL_irq_setEnabled(SIL_IRQ_HANDLE_INVALID, true);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.cancelCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.enableCalls);
}

static void test_clearing_hooks_restores_noop_behavior(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 4;
    TEST_ASSERT_EQUAL_INT32(4, SIL_irq_registerPeriodic(handlerA, 50U, 0U));

    SIL_irq_setHooks(NULL);
    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerPeriodic(handlerA, 50U, 0U));
    TEST_ASSERT_EQUAL_INT32(SIL_IRQ_HANDLE_INVALID, SIL_irq_registerOneShot(handlerA, 2U, 0U));
    SIL_irq_cancel(4);
    SIL_irq_setEnabled(4, false);
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.periodicCalls);   // still just the first call
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.cancelCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.enableCalls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_hooks_register_returns_invalid);
    RUN_TEST(test_no_hooks_cancel_and_enable_are_noops);
    RUN_TEST(test_register_periodic_passes_through);
    RUN_TEST(test_register_one_shot_passes_through);
    RUN_TEST(test_one_shot_accepts_zero_delay);
    RUN_TEST(test_register_rejects_null_handler_and_zero_period_locally);
    RUN_TEST(test_cancel_and_set_enabled_pass_through);
    RUN_TEST(test_cancel_and_enable_guard_invalid_handle);
    RUN_TEST(test_clearing_hooks_restores_noop_behavior);
    return UNITY_END();
}
