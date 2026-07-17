#ifndef SIL_PORTS_H
#define SIL_PORTS_H

/*
 * SIL port registration seam — native sim target only.
 *
 * Sim HW drivers expose "ports": signals they register with the SIL framework
 * at runtime, carrying values in NATIVE units (volts stay volts). The driver
 * owns any conversion to its C-memory representation (e.g. volts -> ADC
 * counts) — the conversion lives where real hardware does it.
 *
 * The framework installs a hook vtable through the control ABI
 * (sil_fw_setHooks, sw/fw/src/sil_fw.h) BEFORE sil_fw_start. Every accessor
 * here is null-safe: with no hooks installed (standalone native runs, Unity
 * tests) register returns SIL_PORTS_HANDLE_INVALID, read returns false, write
 * is a no-op, and a duplex transfer returns false — drivers behave exactly as
 * if the seam did not exist.
 *
 * Two flavors of endpoint:
 *  - a SCALAR port carries a double in native units (read/write); pins are
 *    levels.
 *  - a DUPLEX endpoint is a synchronous serial transaction (buses are
 *    transactions): the driver hands the framework a tx frame and gets the
 *    linked peer's rx frame back within the same call. The framework records
 *    each transaction as :tx / :rx event entries (it appends the modifiers
 *    itself), so a duplex endpoint registers with no modifier and no unit.
 */

#include "lib_types.h"

#define SIL_PORTS_HANDLE_INVALID  (-1)

typedef enum
{
    SIL_PORTS_KIND_SCALAR = 0,
    SIL_PORTS_KIND_DUPLEX = 1,
} SIL_ports_kind_E;

typedef struct
{
    // Opaque framework context, passed back on every call.
    void * context;

    // Register a signal <sigType>:<localName>[:<modifier>] of the given kind,
    // with optional unit. Returns a handle, or a negative value on failure. The
    // framework prefixes its own member instance name — the driver never knows
    // which instance it is (two boards may run the same image).
    int32_t (*registerSignal)(void * context, const char * sigType,
                              const char * localName, const char * modifier,
                              const char * unit, int32_t kind);

    // Read a scalar port's commanded value. Returns false if the signal has
    // never been driven (the driver falls back to its default behavior).
    bool (*readSignal)(void * context, int32_t handle, double * out);

    // Publish a driver-produced value on a scalar port.
    void (*writeSignal)(void * context, int32_t handle, double value);

    // Run a synchronous duplex transaction on a DUPLEX endpoint: pass the tx
    // frame, copy up to rxMax bytes of the linked peer's response into rx, and
    // report the copied count in rxLen. Returns false when the endpoint has no
    // linked peer (the driver fills a floating-bus default).
    bool (*duplexTransfer)(void * context, int32_t handle,
                           const uint8_t * tx, size_t txLen,
                           uint8_t * rx, size_t rxMax, size_t * rxLen);
} SIL_ports_hooks_S;

/* Public Function Declarations */

// Install (copy) the hook vtable, or clear it with NULL. The struct is
// copied, so the caller's pointer need not outlive the call.
void SIL_ports_setHooks(const SIL_ports_hooks_S * const hooks);

// Register a scalar port. modifier is optional (NULL for none); it forms the
// id's :<modifier> segment. Null-safe (see the header comment).
int32_t SIL_ports_register(const char * const sigType, const char * const localName,
                           const char * const modifier, const char * const unit);

// Register a duplex transaction endpoint (no modifier, no unit — the framework
// appends :tx/:rx on the record entries itself). Null-safe.
int32_t SIL_ports_registerDuplex(const char * const sigType, const char * const localName);

bool SIL_ports_read(int32_t handle, double * const out);
void SIL_ports_write(int32_t handle, double value);

// Run a synchronous duplex transaction: tx in, up to rxMax bytes of the peer
// response into rx (count in rxLen). Returns false with no hooks / no linked
// peer / an invalid handle — the caller supplies a floating-bus default.
bool SIL_ports_duplexTransfer(int32_t handle, const uint8_t * const tx, size_t txLen,
                              uint8_t * const rx, size_t rxMax, size_t * const rxLen);

#endif // SIL_PORTS_H
