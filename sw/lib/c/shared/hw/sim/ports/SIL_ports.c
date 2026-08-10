/* Includes */

#include "lib_types.h"

#include "SIL_ports.h"

/* Defines */

/* Typedefs */

typedef struct
{
    SIL_ports_hooks_S hooks;
    bool installed;
} SIL_ports_data_S;

/* Private Function Declarations */

static int32_t SIL_ports_private_register(const char * const sigType, const char * const localName,
                                          const char * const unit, int32_t kind);

/* Private Data Definitions */

static SIL_ports_data_S SIL_ports_data;
static SIL_ports_data_S * const data = &SIL_ports_data;

/* Private Function Definitions */

static int32_t SIL_ports_private_register(const char * const sigType, const char * const localName,
                                          const char * const unit, int32_t kind)
{
    int32_t handle = SIL_PORTS_HANDLE_INVALID;
    if ((data->installed) &&
        (data->hooks.registerSignal != NULL) &&
        (sigType != NULL) &&
        (localName != NULL))
    {
        handle = data->hooks.registerSignal(data->hooks.context, sigType, localName, unit, kind);
    }
    return handle;
}

/* Public Function Definitions */

void SIL_ports_setHooks(const SIL_ports_hooks_S * const hooks)
{
    if (hooks != NULL)
    {
        data->hooks     = *hooks;
        data->installed = true;
    }
    else
    {
        data->hooks     = (SIL_ports_hooks_S){ 0 };
        data->installed = false;
    }
}

int32_t SIL_ports_register(const char * const sigType, const char * const localName,
                           const char * const unit)
{
    return SIL_ports_private_register(sigType, localName, unit, SIL_PORTS_KIND_SCALAR);
}

int32_t SIL_ports_registerDuplex(const char * const sigType, const char * const localName)
{
    return SIL_ports_private_register(sigType, localName, NULL, SIL_PORTS_KIND_DUPLEX);
}

bool SIL_ports_read(int32_t handle, double * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (handle >= 0) &&
        (data->installed) &&
        (data->hooks.readSignal != NULL))
    {
        ret = data->hooks.readSignal(data->hooks.context, handle, out);
    }
    return ret;
}

void SIL_ports_write(int32_t handle, double value)
{
    if ((handle >= 0) &&
        (data->installed) &&
        (data->hooks.writeSignal != NULL))
    {
        data->hooks.writeSignal(data->hooks.context, handle, value);
    }
}

bool SIL_ports_duplexTransfer(int32_t handle, const uint8_t * const tx, size_t txLen,
                              uint8_t * const rx, size_t rxMax, size_t * const rxLen)
{
    bool ret = false;
    if ((rx != NULL) &&
        (rxLen != NULL) &&
        (handle >= 0) &&
        (data->installed) &&
        (data->hooks.duplexTransfer != NULL))
    {
        ret = data->hooks.duplexTransfer(data->hooks.context, handle, tx, txLen, rx, rxMax, rxLen);
    }
    return ret;
}
