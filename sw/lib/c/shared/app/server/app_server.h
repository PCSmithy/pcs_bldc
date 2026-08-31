#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_COBSFrame.h"
#include "IO_serial.h"

#include "lib_protobuf_config.h"   // project schema bindings: shared_/board_ types

/* Defines */

// Ring storage overhead past the budget: the worst tick's record header plus
// the ring's one empty slot, so every list the u-formula admits fully fits.
#define APP_SERVER_TRACE_RING_OVERHEAD_BYTES (3U)
#define APP_SERVER_TRACE_STORAGE_BYTES(budgetBytes) \
    ((budgetBytes) + APP_SERVER_TRACE_RING_OVERHEAD_BYTES)

/* Typedefs */

// One span of protocol-addressable memory (fw~conn_trace_001). base is the
// backing storage for protocol address `start` — equal to `start` on hardware,
// a host allocation in sim/tests.
typedef struct
{
    uint32_t start;    // first protocol address
    uint32_t length;   // bytes
    uintptr_t base;    // backing memory of `start`
} app_server_region_S;

// One watch-table entry; location is the resolved backing pointer.
typedef struct
{
    uintptr_t location;
    uint8_t sizeBytes;   // 1..8
    uint8_t period_ms;   // 1, 10, or 100
} app_server_watch_S;

typedef struct
{
    IO_COBSFrame_channel_E frame;   // framed protocol channel served
    IO_serial_channel_E serial;     // underlying port, for connection gating

    // Trace-service resources (fw~conn_trace_001). The board owns the RAM:
    // watchStorage holds 2 * watchCapacity entries (active + staged halves);
    // sampleStorage holds APP_SERVER_TRACE_STORAGE_BYTES(sampleRamBudgetBytes).
    const app_server_region_S * readableRegions;
    uint32_t readableRegionCount;
    const app_server_region_S * writableRegions;
    uint32_t writableRegionCount;
    app_server_watch_S * watchStorage;
    uint32_t watchCapacity;
    uint8_t * sampleStorage;
    uint32_t sampleRamBudgetBytes;
    uint32_t linkBudgetBytesPerS;

    // Board hooks behind the envelope's two extension payloads; either may be
    // NULL on a board without commands / telemetry.
    // Fill the Response verdict: accepted, or rejected with a cause.
    void (*handleRequest)(const board_Request * const request, shared_Response * const response);
    // Fill the periodic board.Telemetry; false skips this period.
    bool (*buildTelemetry)(board_Telemetry * const telemetry);
} app_server_config_S;

/* Public Data Declarations */

extern const app_server_config_S app_server_config;

/* Public Function Declarations */

bool app_server_init(const app_server_config_S * const config);

// call from server task
void app_server_run1ms(void);

// call from periodic 1ms task
void app_server_sample1ms(void);

// Capture one byte of standard-output text for the log stream.
void app_server_logByte(uint8_t byte);
