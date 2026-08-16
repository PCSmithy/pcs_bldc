#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_COBSFrame.h"
#include "IO_serial.h"

#include "lib_protobuf_config.h"   // project schema bindings: shared_/board_ types

/* Typedefs */

typedef struct
{
    IO_COBSFrame_channel_E frame;   // framed protocol channel served
    IO_serial_channel_E serial;     // underlying port, for connection gating

    // Board hooks — the board-specific logic behind the envelope's two
    // extension payloads (shared.proto fields 60/61). Either may be NULL on a
    // board without commands / telemetry.
    //
    // handleRequest: consume a decoded board.Request, fill the framework
    // Response verdict (accepted, or rejected with a cause).
    void (*handleRequest)(const board_Request * const request, shared_Response * const response);
    // buildTelemetry: fill the periodic board.Telemetry; false skips this
    // period's publication.
    bool (*buildTelemetry)(board_Telemetry * const telemetry);
} app_server_config_S;

/* Public Data Declarations */

extern const app_server_config_S app_server_config;

/* Public Function Declarations */

bool app_server_init(const app_server_config_S * const config);

// One server pass: answer received requests, publish telemetry every 100th
// call, drain captured log text. Call at 1 ms period.
void app_server_run1ms(void);

// Capture one byte of standard-output text for the log stream.
void app_server_logByte(uint8_t byte);
