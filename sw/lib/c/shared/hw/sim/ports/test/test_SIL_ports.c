#include "SIL_ports.h"
#include "unity.h"

// Call log for the fake vtable, so pass-through of every argument is provable.
typedef struct
{
    void *  lastContext;
    char    lastSigType[32];
    char    lastLocalName[32];
    char    lastUnit[32];
    int32_t lastKind;
    int32_t registerReturn;

    int32_t lastReadHandle;
    double  readValue;
    bool    readReturn;

    int32_t lastWriteHandle;
    double  lastWriteValue;
    uint32_t writeCalls;

    int32_t  lastDuplexHandle;
    uint8_t  lastDuplexTx[8];
    size_t   lastDuplexTxLen;
    uint8_t  duplexResp[8];
    size_t   duplexRespLen;
    bool     duplexReturn;
    uint32_t duplexCalls;
} fakeHooksLog_S;

static fakeHooksLog_S fakeLog;
static uint32_t contextTag;   // any stable address to use as the context

static void copyStr(char * const dst, const char * const src, size_t dstSize)
{
    size_t i = 0U;
    if (src != NULL)
    {
        while ((src[i] != '\0') && (i < (dstSize - 1U)))
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int32_t fakeRegister(void * context, const char * sigType,
                            const char * localName, const char * unit, int32_t kind)
{
    fakeLog.lastContext = context;
    copyStr(fakeLog.lastSigType, sigType, sizeof(fakeLog.lastSigType));
    copyStr(fakeLog.lastLocalName, localName, sizeof(fakeLog.lastLocalName));
    copyStr(fakeLog.lastUnit, unit, sizeof(fakeLog.lastUnit));
    fakeLog.lastKind = kind;
    return fakeLog.registerReturn;
}

static bool fakeRead(void * context, int32_t handle, double * out)
{
    fakeLog.lastContext    = context;
    fakeLog.lastReadHandle = handle;
    if (fakeLog.readReturn)
    {
        *out = fakeLog.readValue;
    }
    return fakeLog.readReturn;
}

static void fakeWrite(void * context, int32_t handle, double value)
{
    fakeLog.lastContext     = context;
    fakeLog.lastWriteHandle = handle;
    fakeLog.lastWriteValue  = value;
    fakeLog.writeCalls++;
}

static bool fakeDuplex(void * context, int32_t handle, const uint8_t * tx, size_t txLen,
                       uint8_t * rx, size_t rxMax, size_t * rxLen)
{
    fakeLog.lastContext      = context;
    fakeLog.lastDuplexHandle = handle;
    fakeLog.duplexCalls++;
    fakeLog.lastDuplexTxLen = (txLen < sizeof(fakeLog.lastDuplexTx)) ? txLen : sizeof(fakeLog.lastDuplexTx);
    for (size_t i = 0U; i < fakeLog.lastDuplexTxLen; i++)
    {
        fakeLog.lastDuplexTx[i] = tx[i];
    }
    size_t n = 0U;
    if (fakeLog.duplexReturn)
    {
        n = (fakeLog.duplexRespLen < rxMax) ? fakeLog.duplexRespLen : rxMax;
        for (size_t i = 0U; i < n; i++)
        {
            rx[i] = fakeLog.duplexResp[i];
        }
    }
    *rxLen = n;
    return fakeLog.duplexReturn;
}

static void installFakeHooks(void)
{
    const SIL_ports_hooks_S hooks = {
        .context        = &contextTag,
        .registerSignal = fakeRegister,
        .readSignal     = fakeRead,
        .writeSignal    = fakeWrite,
        .duplexTransfer = fakeDuplex,
    };
    SIL_ports_setHooks(&hooks);
}

void setUp(void)
{
    SIL_ports_setHooks(NULL);
    fakeLog = (fakeHooksLog_S){ 0 };
}

void tearDown(void)
{
    SIL_ports_setHooks(NULL);
}

/* ---- no hooks installed: everything is a safe no-op ---- */

static void test_no_hooks_register_returns_invalid(void)
{
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID,
                            SIL_ports_register("vsig", "adc_in", "V"));
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID,
                            SIL_ports_registerDuplex("spi", "AS5048_1"));
}

static void test_no_hooks_read_returns_false(void)
{
    double out = 42.0;
    TEST_ASSERT_FALSE(SIL_ports_read(0, &out));
    // Exact compare is safe: the helper is a pure pass-through (and Unity's
    // double-precision asserts are disabled in this build).
    TEST_ASSERT_TRUE(out == 42.0);   // out untouched
}

static void test_no_hooks_write_is_noop(void)
{
    SIL_ports_write(0, 1.5);   // must not crash
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.writeCalls);
}

static void test_no_hooks_duplex_transfer_returns_false(void)
{
    const uint8_t tx[2] = { 0xFFU, 0xFFU };
    uint8_t rx[2]  = { 1U, 2U };
    size_t  rxLen  = 99U;
    TEST_ASSERT_FALSE(SIL_ports_duplexTransfer(0, tx, 2U, rx, sizeof(rx), &rxLen));
}

/* ---- hooks installed: arguments pass straight through ---- */

static void test_register_passes_through(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 7;

    TEST_ASSERT_EQUAL_INT32(7, SIL_ports_register("vsig", "ADC1_IN6", "V"));
    TEST_ASSERT_EQUAL_PTR(&contextTag, fakeLog.lastContext);
    TEST_ASSERT_EQUAL_STRING("vsig", fakeLog.lastSigType);
    TEST_ASSERT_EQUAL_STRING("ADC1_IN6", fakeLog.lastLocalName);
    TEST_ASSERT_EQUAL_STRING("V", fakeLog.lastUnit);
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_KIND_SCALAR, fakeLog.lastKind);
}

static void test_register_duplex_passes_kind_no_unit(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 3;

    TEST_ASSERT_EQUAL_INT32(3, SIL_ports_registerDuplex("spi", "AS5048_1"));
    TEST_ASSERT_EQUAL_STRING("spi", fakeLog.lastSigType);
    TEST_ASSERT_EQUAL_STRING("AS5048_1", fakeLog.lastLocalName);
    TEST_ASSERT_EQUAL_STRING("", fakeLog.lastUnit);     // NULL unit -> empty
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_KIND_DUPLEX, fakeLog.lastKind);
}

static void test_register_rejects_null_names_locally(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 7;
    // NULL sigType/localName never reach the hook.
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID, SIL_ports_register(NULL, "x", "V"));
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID, SIL_ports_register("vsig", NULL, "V"));
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID, SIL_ports_registerDuplex(NULL, "x"));
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID, SIL_ports_registerDuplex("spi", NULL));
}

static void test_read_passes_through_when_driven(void)
{
    installFakeHooks();
    fakeLog.readReturn = true;
    fakeLog.readValue  = 1.65;

    double out = 0.0;
    TEST_ASSERT_TRUE(SIL_ports_read(3, &out));
    TEST_ASSERT_EQUAL_INT32(3, fakeLog.lastReadHandle);
    TEST_ASSERT_TRUE(out == 1.65);   // pure pass-through: exact compare is safe
}

static void test_read_false_when_never_driven(void)
{
    installFakeHooks();
    fakeLog.readReturn = false;

    double out = 0.0;
    TEST_ASSERT_FALSE(SIL_ports_read(3, &out));
}

static void test_read_guards_invalid_handle_and_null_out(void)
{
    installFakeHooks();
    fakeLog.readReturn = true;

    double out = 0.0;
    TEST_ASSERT_FALSE(SIL_ports_read(SIL_PORTS_HANDLE_INVALID, &out));
    TEST_ASSERT_FALSE(SIL_ports_read(3, NULL));
}

static void test_write_passes_through(void)
{
    installFakeHooks();
    SIL_ports_write(5, 2.75);
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.writeCalls);
    TEST_ASSERT_EQUAL_INT32(5, fakeLog.lastWriteHandle);
    TEST_ASSERT_TRUE(fakeLog.lastWriteValue == 2.75);   // pure pass-through
}

static void test_write_guards_invalid_handle(void)
{
    installFakeHooks();
    SIL_ports_write(SIL_PORTS_HANDLE_INVALID, 2.75);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.writeCalls);
}

static void test_duplex_transfer_passes_through(void)
{
    installFakeHooks();
    fakeLog.duplexReturn  = true;
    fakeLog.duplexResp[0] = 0x90U;
    fakeLog.duplexResp[1] = 0x00U;
    fakeLog.duplexRespLen = 2U;

    const uint8_t tx[2] = { 0xFFU, 0xFFU };
    uint8_t rx[2]  = { 0U, 0U };
    size_t  rxLen  = 0U;
    TEST_ASSERT_TRUE(SIL_ports_duplexTransfer(4, tx, 2U, rx, sizeof(rx), &rxLen));
    TEST_ASSERT_EQUAL_UINT32(1U, fakeLog.duplexCalls);
    TEST_ASSERT_EQUAL_INT32(4, fakeLog.lastDuplexHandle);
    TEST_ASSERT_EQUAL_UINT(2U, fakeLog.lastDuplexTxLen);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, fakeLog.lastDuplexTx[0]);
    TEST_ASSERT_EQUAL_UINT(2U, rxLen);
    TEST_ASSERT_EQUAL_HEX8(0x90U, rx[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, rx[1]);
}

static void test_duplex_transfer_guards_invalid_handle_and_null(void)
{
    installFakeHooks();
    fakeLog.duplexReturn = true;

    const uint8_t tx[2] = { 0xFFU, 0xFFU };
    uint8_t rx[2]  = { 0U, 0U };
    size_t  rxLen  = 0U;
    // Invalid handle / NULL rx / NULL rxLen never reach the hook.
    TEST_ASSERT_FALSE(SIL_ports_duplexTransfer(SIL_PORTS_HANDLE_INVALID, tx, 2U, rx, sizeof(rx), &rxLen));
    TEST_ASSERT_FALSE(SIL_ports_duplexTransfer(4, tx, 2U, NULL, sizeof(rx), &rxLen));
    TEST_ASSERT_FALSE(SIL_ports_duplexTransfer(4, tx, 2U, rx, sizeof(rx), NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.duplexCalls);
}

static void test_clearing_hooks_restores_noop_behavior(void)
{
    installFakeHooks();
    fakeLog.registerReturn = 7;
    TEST_ASSERT_EQUAL_INT32(7, SIL_ports_register("vsig", "x", NULL));

    SIL_ports_setHooks(NULL);
    TEST_ASSERT_EQUAL_INT32(SIL_PORTS_HANDLE_INVALID, SIL_ports_register("vsig", "x", NULL));
    double out = 0.0;
    TEST_ASSERT_FALSE(SIL_ports_read(7, &out));
    SIL_ports_write(7, 1.0);
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.writeCalls);
    const uint8_t tx[1] = { 0U };
    uint8_t rx[1]  = { 0U };
    size_t  rxLen  = 0U;
    TEST_ASSERT_FALSE(SIL_ports_duplexTransfer(0, tx, 1U, rx, sizeof(rx), &rxLen));
    TEST_ASSERT_EQUAL_UINT32(0U, fakeLog.duplexCalls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_hooks_register_returns_invalid);
    RUN_TEST(test_no_hooks_read_returns_false);
    RUN_TEST(test_no_hooks_write_is_noop);
    RUN_TEST(test_no_hooks_duplex_transfer_returns_false);
    RUN_TEST(test_register_passes_through);
    RUN_TEST(test_register_duplex_passes_kind_no_unit);
    RUN_TEST(test_register_rejects_null_names_locally);
    RUN_TEST(test_read_passes_through_when_driven);
    RUN_TEST(test_read_false_when_never_driven);
    RUN_TEST(test_read_guards_invalid_handle_and_null_out);
    RUN_TEST(test_write_passes_through);
    RUN_TEST(test_write_guards_invalid_handle);
    RUN_TEST(test_duplex_transfer_passes_through);
    RUN_TEST(test_duplex_transfer_guards_invalid_handle_and_null);
    RUN_TEST(test_clearing_hooks_restores_noop_behavior);
    return UNITY_END();
}
