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
 * tests) register returns SIL_PORTS_HANDLE_INVALID, read returns false, and
 * write is a no-op — drivers behave exactly as if the seam did not exist.
 *
 * Values are double scalars for now. Extension path: typed variants
 * (registerSignalTyped / read/write per type) are added to the vtable when
 * needed; transport/event payloads arrive with the comms (D5) work, not here.
 */

#include "lib_types.h"

#define SIL_PORTS_HANDLE_INVALID  (-1)

typedef struct
{
    // Opaque framework context, passed back on every call.
    void * context;

    // Register a signal named <localName> of <sigType> (e.g. "vsig") with
    // optional unit. Returns a handle, or a negative value on failure. The
    // framework prefixes its own member instance name — the driver never
    // knows which instance it is (two boards may run the same image).
    int32_t (*registerSignal)(void * context, const char * sigType,
                              const char * localName, const char * unit);

    // Read a port's commanded value. Returns false if the signal has never
    // been driven (the driver falls back to its default behavior).
    bool (*readSignal)(void * context, int32_t handle, double * out);

    // Publish a driver-produced value on a port.
    void (*writeSignal)(void * context, int32_t handle, double value);
} SIL_ports_hooks_S;

/* Public Function Declarations */

// Install (copy) the hook vtable, or clear it with NULL. The struct is
// copied, so the caller's pointer need not outlive the call.
void SIL_ports_setHooks(const SIL_ports_hooks_S * const hooks);

// Null-safe wrappers around the vtable (see the header comment).
int32_t SIL_ports_register(const char * const sigType, const char * const localName,
                           const char * const unit);
bool SIL_ports_read(int32_t handle, double * const out);
void SIL_ports_write(int32_t handle, double value);

#endif // SIL_PORTS_H
