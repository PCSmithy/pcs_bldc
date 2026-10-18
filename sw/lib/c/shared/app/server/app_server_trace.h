#pragma once

/* Includes */
#include "lib_types.h"

#include "app_server.h"

#include "pb_decode.h"   // pb_istream_t / pb_field_t for the decode callbacks

#ifdef __cplusplus
extern "C" {
#endif

/* Defines */

// Per-message wire overhead W (fw~conn_trace_005): the envelope's tags and
// lengths, the period/first-cycle/count varints, CRC-32, COBS expansion, and
// the frame delimiters, worst case.
#define APP_SERVER_TRACE_WIRE_OVERHEAD_BYTES (27U)

// Ring record layout: 3-byte header (length, group) + 4-byte cycle index + data.
#define APP_SERVER_TRACE_RECORD_HEADER_BYTES   (3U)
#define APP_SERVER_TRACE_CYCLE_BYTES           (4U)
#define APP_SERVER_TRACE_RECORD_OVERHEAD_BYTES (APP_SERVER_TRACE_RECORD_HEADER_BYTES + APP_SERVER_TRACE_CYCLE_BYTES)

/* Public Function Declarations */

bool app_server_trace_init(const app_server_config_S * const config);

// Producer side: capture every group the cycle falls due for into the sample
// ring. Runs in the bridge cycle callback's ISR context.
void app_server_trace_sampleCycle(void);

// Drop the active watch list, buffered records, and cycle index.
void app_server_trace_clear(void);

// Envelope-level submsg_callback hook: installed on shared_Envelope.cb_payload
// before decode, arms the WatchRequest.watches entry consumer when that oneof
// arm is chosen (nanopb wipes the union first, so callbacks inside it cannot
// be pre-set).
bool app_server_trace_envelopeCallback(pb_istream_t * stream, const pb_field_t * field, void ** arg);

// Admit the staged watch list (fw~conn_trace_002): true installs it and fills
// status; false leaves the active list unchanged and fills response's cause.
bool app_server_trace_admit(trace_TraceStatus * const status, shared_Response * const response);

// Fill the trace capability report for the active list (fw~conn_trace_006).
void app_server_trace_status(trace_TraceStatus * const status);

// One-shot read (fw~conn_trace_007): true fills reply; false fills the cause.
bool app_server_trace_read(const trace_ReadRequest * const request,
                           trace_ReadReply * const reply,
                           shared_Response * const response);

// One-shot write (fw~conn_trace_008): true after writing; false fills the cause.
bool app_server_trace_write(const trace_WriteRequest * const request,
                            shared_Response * const response);

// Cycles between one group's records (fw~conn_trace_004); zero past the last
// group.
uint32_t app_server_trace_groupPeriodCycles(uint32_t group);

// Consumer side.

// Bytes buffered right now: each record's data plus its overhead.
uint32_t app_server_trace_bufferedBytes(void);

// Moves the run of records at the ring's tail out as one block: the oldest
// record, then following records of its group whose cycle indices step by
// the group's period, each whole, while the data fits maxBytes and the
// records lie within *ringBudget bytes of the ring (a pass's snapshot, which
// the call reduces by what it took). Returns the record count, with the
// run's group, first cycle index, and data length; zero takes nothing.
uint32_t app_server_trace_popRun(uint8_t * const buffer, size_t maxBytes, uint32_t * const ringBudget,
                                 uint32_t * const group, uint32_t * const firstCycle, size_t * const dataLen);
#ifdef __cplusplus
}
#endif
