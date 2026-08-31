#pragma once

/* Includes */
#include "lib_types.h"

#include "app_server.h"

#include "pb_decode.h"   // pb_istream_t / pb_field_t for the decode callbacks

/* Defines */

// per-message overhead: envelope tags/lengths + tick varint + CRC-32 + COBS
// expansion + frame delimiters, worst case.
#define APP_SERVER_TRACE_WIRE_OVERHEAD_BYTES (21U)

/* Public Function Declarations */

bool app_server_trace_init(const app_server_config_S * const config);

// Producer side: capture the active list's due entries for one tick into the
// sample ring. Call from the 1 ms control task, after the control update.
void app_server_trace_sample1ms(void);

// Drop the active watch list, buffered samples, and tick count.
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

// Consumer side: data length of the oldest buffered tick record, or false
// when the ring is empty. Pop moves that record's tick count and data out.
bool app_server_trace_peekLen(size_t * const dataLen);
bool app_server_trace_pop(uint32_t * const tick,
                          uint8_t * const buffer, size_t bufferLen,
                          size_t * const dataLen);
