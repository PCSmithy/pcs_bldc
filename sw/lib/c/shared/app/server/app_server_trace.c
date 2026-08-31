/* Includes */
#include "app_server_trace.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* Defines */

// Ring record layout: [dataLen u16 LE][tick u32 LE][data...]. The 6 header
// bytes are ring bookkeeping only; they never ride the wire.
#define APP_SERVER_TRACE_RECORD_HEADER_BYTES (2U)
#define APP_SERVER_TRACE_TICK_BYTES          (4U)

#define APP_SERVER_TRACE_MAX_TICK_DATA_BYTES (sizeof(((trace_Samples *) 0)->data.bytes))
#define APP_SERVER_TRACE_MAX_WATCH_BYTES     (8U)
#define APP_SERVER_TRACE_MAX_READ_BYTES      (sizeof(((trace_ReadReply *) 0)->data.bytes))
#define APP_SERVER_TRACE_MAX_WRITE_BYTES     (sizeof(((trace_WriteRequest *) 0)->data.bytes))

// SPSC ring fences: order the record's plain data accesses against the
// volatile index publishes, so the ring stays correct with an ISR-context
// producer or off a single core (DMB on Cortex-M; compiler-only on x86 TSO).
// ACQUIRE after reading the peer's index, before touching the buffer;
// RELEASE after touching the buffer, before publishing our index.
#define APP_SERVER_TRACE_BARRIER_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define APP_SERVER_TRACE_BARRIER_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)

// The board's storage macro must cover exactly the record header + empty slot.
_Static_assert(APP_SERVER_TRACE_RING_OVERHEAD_BYTES ==
                   (APP_SERVER_TRACE_RECORD_HEADER_BYTES + 1U),
               "ring overhead mismatch");

/* Private Data Definitions */

typedef struct
{
    const app_server_config_S * config;
    // Watch double buffer: the sampler streams `active`; WatchRequest decode
    // fills `staged`; an accepted admission swaps the two under a critical
    // section so the higher-priority sampler never sees a half-committed list.
    app_server_watch_S * active;
    app_server_watch_S * staged;
    uint32_t activeCount;
    uint32_t stagedCount;
    bool stagedRejected;
    char stagedCause[sizeof(((shared_Response *) 0)->cause)];
    uint32_t tick;
    // Sample ring: SPSC while streaming (sampler owns head, server owns tail);
    // cross-writer resets only inside a critical section excluding the sampler.
    volatile uint32_t head;
    volatile uint32_t tail;
} app_server_trace_data_S;

static app_server_trace_data_S app_server_trace_data;
static app_server_trace_data_S * const data = &app_server_trace_data;

/* Private Function Definitions */

// Physical ring size: the budget plus header + one empty slot (full and empty
// stay distinguishable), so a worst tick at u == budget still fits.
static uint32_t app_server_trace_private_capacity(void)
{
    return data->config->sampleRamBudgetBytes + APP_SERVER_TRACE_RING_OVERHEAD_BYTES;
}

static uint32_t app_server_trace_private_ringUsed(void)
{
    const uint32_t capacity = app_server_trace_private_capacity();
    const uint32_t head = data->head;
    const uint32_t tail = data->tail;
    return ((head >= tail)) ? (head - tail) : ((head + capacity) - tail);
}

// Write len bytes at the rolling ring index; returns the advanced index. The
// caller publishes data->head only after the whole record is written.
static uint32_t app_server_trace_private_ringWrite(uint32_t index, const uint8_t * const bytes, uint32_t len)
{
    const uint32_t capacity = app_server_trace_private_capacity();
    uint8_t * const storage = data->config->sampleStorage;
    for (uint32_t i = 0U; i < len; i++)
    {
        storage[index] = bytes[i];
        index++;
        if (index >= capacity)
        {
            index = 0U;
        }
    }
    return index;
}

// Byte at `offset` past the tail, without consuming (offset < capacity).
static uint8_t app_server_trace_private_ringPeek(uint32_t offset)
{
    const uint32_t capacity = app_server_trace_private_capacity();
    uint32_t index = data->tail + offset;
    if (index >= capacity)
    {
        index -= capacity;
    }
    return data->config->sampleStorage[index];
}

// Resolve a protocol span to its backing memory: contained in one region, or
// false. Phrased to avoid address-arithmetic overflow.
static bool app_server_trace_private_resolve(const app_server_region_S * const regions,
                                             uint32_t regionCount,
                                             uint32_t address, uint32_t size,
                                             uintptr_t * const location)
{
    bool ret = false;
    for (uint32_t i = 0U; i < regionCount; i++)
    {
        const app_server_region_S * const region = &regions[i];
        if ((size <= region->length) &&
            (address >= region->start) &&
            ((address - region->start) <= (region->length - size)))
        {
            *location = region->base + (uintptr_t) (address - region->start);
            ret = true;
            break;
        }
    }
    return ret;
}

static bool app_server_trace_private_regionsValid(const app_server_region_S * const regions, uint32_t regionCount)
{
    bool ret = true;
    for (uint32_t i = 0U; i < regionCount; i++)
    {
        if (regions[i].length == 0U)
        {
            ret = false;
            break;
        }
    }
    return ret;
}

// First failure wins: the cause of the earliest bad entry survives to the
// rejection Response.
static void app_server_trace_private_stageReject(const char * const cause)
{
    if (!data->stagedRejected)
    {
        data->stagedRejected = true;
        (void) strcpy(data->stagedCause, cause);
    }
}

// [impl->fw~conn_trace_002~1] per-entry checks of the admission table
static void app_server_trace_private_stageEntry(uint32_t address, uint32_t size, uint32_t period)
{
    uintptr_t location = 0U;
    if ((size < 1U) || (size > APP_SERVER_TRACE_MAX_WATCH_BYTES))
    {
        app_server_trace_private_stageReject("watch size outside 1..8");
    }
    else if ((period != 1U) && (period != 10U) && (period != 100U))
    {
        app_server_trace_private_stageReject("watch period not 1/10/100 ms");
    }
    else if (!app_server_trace_private_resolve(data->config->readableRegions,
                                               data->config->readableRegionCount,
                                               address, size, &location))
    {
        app_server_trace_private_stageReject("watch span not readable");
    }
    else if (data->stagedCount >= data->config->watchCapacity)
    {
        app_server_trace_private_stageReject("watch list exceeds capacity");
    }
    else
    {
        data->staged[data->stagedCount] = (app_server_watch_S) {
            .location  = location,
            .sizeBytes = (uint8_t) size,
            .period_ms = (uint8_t) period,
        };
        data->stagedCount++;
    }
}

// nanopb callback: one repeated WatchRequest.watches element per call.
static bool app_server_trace_private_watchEntryCallback(pb_istream_t * stream, const pb_field_t * field, void ** arg)
{
    (void) field;
    (void) arg;
    trace_Watch watch = trace_Watch_init_zero;
    const bool ret = pb_decode(stream, trace_Watch_fields, &watch);
    if (ret)
    {
        app_server_trace_private_stageEntry(watch.address, watch.size, watch.period_ms);
    }
    return ret;
}

// The admission formulas of fw~conn_trace_002: worst-tick data bytes, the
// worst-tick RAM figure u, and the link rate r (zero usage for an empty list).
static void app_server_trace_private_usage(const app_server_watch_S * const list, uint32_t count,
                                           uint32_t * const dataBytes,
                                           uint32_t * const ramWorstTick,
                                           uint32_t * const linkRate)
{
    uint32_t sumBytes = 0U;
    uint32_t rate = 0U;
    uint32_t maxFreq = 0U;
    for (uint32_t i = 0U; i < count; i++)
    {
        const uint32_t freq = 1000U / (uint32_t) list[i].period_ms;
        sumBytes += list[i].sizeBytes;
        rate += ((uint32_t) list[i].sizeBytes) * freq;
        if (freq > maxFreq)
        {
            maxFreq = freq;
        }
    }
    *dataBytes = sumBytes;
    *ramWorstTick = ((count > 0U)) ? (APP_SERVER_TRACE_TICK_BYTES + sumBytes) : 0U;
    *linkRate = rate + (APP_SERVER_TRACE_WIRE_OVERHEAD_BYTES * maxFreq);
}

/* Public Function Definitions */

// [impl->fw~conn_trace_001~1]
bool app_server_trace_init(const app_server_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->readableRegions != NULL) &&
        (config->readableRegionCount > 0U) &&
        ((config->writableRegions != NULL) || (config->writableRegionCount == 0U)) &&
        (config->watchStorage != NULL) &&
        (config->watchCapacity > 0U) &&
        (config->sampleStorage != NULL) &&
        (config->sampleRamBudgetBytes > 0U) &&
        (config->linkBudgetBytesPerS > 0U) &&
        (app_server_trace_private_regionsValid(config->readableRegions, config->readableRegionCount)) &&
        (app_server_trace_private_regionsValid(config->writableRegions, config->writableRegionCount)))
    {
        data->config = config;
        data->active = &config->watchStorage[0U];
        data->staged = &config->watchStorage[config->watchCapacity];
        data->activeCount = 0U;
        data->stagedCount = 0U;
        data->stagedRejected = false;
        data->stagedCause[0] = '\0';
        data->tick = 0U;
        data->head = 0U;
        data->tail = 0U;
        ret = true;
    }
    return ret;
}

// [impl->fw~conn_trace_004~1]
void app_server_trace_sample1ms(void)
{
    if ((data->config != NULL) && (data->activeCount > 0U))
    {
        const uint32_t tick = data->tick;
        data->tick = tick + 1U;

        uint32_t dataLen = 0U;
        for (uint32_t i = 0U; i < data->activeCount; i++)
        {
            if ((tick % ((uint32_t) data->active[i].period_ms)) == 0U)
            {
                dataLen += data->active[i].sizeBytes;
            }
        }

        if (dataLen > 0U)
        {
            const uint32_t capacity = app_server_trace_private_capacity();
            const uint32_t needed = APP_SERVER_TRACE_RECORD_HEADER_BYTES +
                                    APP_SERVER_TRACE_TICK_BYTES + dataLen;
            const uint32_t freeBytes = (capacity - 1U) - app_server_trace_private_ringUsed();
            // A tick that does not fit is skipped whole; the tick-count gap
            // is the host's drop signal.
            if (needed <= freeBytes)
            {
                // The free-space check read tail: fence before reusing space
                // the consumer just released.
                APP_SERVER_TRACE_BARRIER_ACQUIRE();
                const uint8_t header[APP_SERVER_TRACE_RECORD_HEADER_BYTES] = {
                    (uint8_t) (dataLen & 0xFFU),
                    (uint8_t) ((dataLen >> 8U) & 0xFFU),
                };
                const uint8_t tickBytes[APP_SERVER_TRACE_TICK_BYTES] = {
                    (uint8_t) (tick & 0xFFU),
                    (uint8_t) ((tick >> 8U) & 0xFFU),
                    (uint8_t) ((tick >> 16U) & 0xFFU),
                    (uint8_t) ((tick >> 24U) & 0xFFU),
                };
                uint32_t index = data->head;
                index = app_server_trace_private_ringWrite(index, header, APP_SERVER_TRACE_RECORD_HEADER_BYTES);
                index = app_server_trace_private_ringWrite(index, tickBytes, APP_SERVER_TRACE_TICK_BYTES);
                for (uint32_t i = 0U; i < data->activeCount; i++)
                {
                    if ((tick % ((uint32_t) data->active[i].period_ms)) == 0U)
                    {
                        index = app_server_trace_private_ringWrite(index,
                                                                   (const uint8_t *) data->active[i].location,
                                                                   data->active[i].sizeBytes);
                    }
                }
                // Publish last: the consumer never sees a partial record.
                APP_SERVER_TRACE_BARRIER_RELEASE();
                data->head = index;
            }
        }
    }
}

// [impl->fw~conn_trace_003~1]
void app_server_trace_clear(void)
{
    if (data->config != NULL)
    {
        taskENTER_CRITICAL();
        data->activeCount = 0U;
        data->tick = 0U;
        data->head = 0U;
        data->tail = 0U;
        taskEXIT_CRITICAL();
    }
}

bool app_server_trace_envelopeCallback(pb_istream_t * stream, const pb_field_t * field, void ** arg)
{
    (void) stream;
    (void) arg;
    if ((data->config != NULL) && (field->tag == shared_Envelope_watch_request_tag))
    {
        trace_WatchRequest * const request = (trace_WatchRequest *) field->pData;
        request->watches.funcs.decode = app_server_trace_private_watchEntryCallback;
        request->watches.arg = NULL;
        data->stagedCount = 0U;
        data->stagedRejected = false;
        data->stagedCause[0] = '\0';
    }
    return true;
}

// [impl->fw~conn_trace_002~1]
bool app_server_trace_admit(trace_TraceStatus * const status, shared_Response * const response)
{
    bool ret = false;
    if ((data->config != NULL) && (status != NULL) && (response != NULL))
    {
        uint32_t dataBytes = 0U;
        uint32_t ramWorstTick = 0U;
        uint32_t linkRate = 0U;
        app_server_trace_private_usage(data->staged, data->stagedCount,
                                       &dataBytes, &ramWorstTick, &linkRate);

        if (data->stagedRejected)
        {
            (void) strcpy(response->cause, data->stagedCause);
        }
        else if (dataBytes > APP_SERVER_TRACE_MAX_TICK_DATA_BYTES)
        {
            (void) strcpy(response->cause, "exceeds Samples data capacity");
        }
        else if (ramWorstTick > data->config->sampleRamBudgetBytes)
        {
            (void) strcpy(response->cause, "exceeds sample-RAM budget");
        }
        else if (linkRate > data->config->linkBudgetBytesPerS)
        {
            (void) strcpy(response->cause, "exceeds link budget");
        }
        else
        {
            // Commit: swap the halves and restart the stream, atomically
            // against the sampler.
            taskENTER_CRITICAL();
            app_server_watch_S * const previousActive = data->active;
            data->active = data->staged;
            data->staged = previousActive;
            data->activeCount = data->stagedCount;
            data->tick = 0U;
            data->head = 0U;
            data->tail = 0U;
            taskEXIT_CRITICAL();
            app_server_trace_status(status);
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~conn_trace_006~1]
void app_server_trace_status(trace_TraceStatus * const status)
{
    if ((data->config != NULL) && (status != NULL))
    {
        uint32_t dataBytes = 0U;
        uint32_t ramWorstTick = 0U;
        uint32_t linkRate = 0U;
        app_server_trace_private_usage(data->active, data->activeCount,
                                       &dataBytes, &ramWorstTick, &linkRate);
        status->ram_budget_bytes        = data->config->sampleRamBudgetBytes;
        status->ram_worst_tick_bytes    = ramWorstTick;
        status->link_budget_bytes_per_s = data->config->linkBudgetBytesPerS;
        status->link_rate_bytes_per_s   = linkRate;
    }
}

// [impl->fw~conn_trace_007~1]
bool app_server_trace_read(const trace_ReadRequest * const request,
                           trace_ReadReply * const reply,
                           shared_Response * const response)
{
    bool ret = false;
    if ((data->config != NULL) && (request != NULL) && (reply != NULL) && (response != NULL))
    {
        uintptr_t location = 0U;
        if ((request->size < 1U) || (request->size > APP_SERVER_TRACE_MAX_READ_BYTES))
        {
            (void) strcpy(response->cause, "read size outside 1..128");
        }
        else if (!app_server_trace_private_resolve(data->config->readableRegions,
                                                   data->config->readableRegionCount,
                                                   request->address, request->size, &location))
        {
            (void) strcpy(response->cause, "read span not readable");
        }
        else
        {
            (void) memcpy(reply->data.bytes, (const void *) location, request->size);
            reply->data.size = (pb_size_t) request->size;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~conn_trace_008~1]
bool app_server_trace_write(const trace_WriteRequest * const request,
                            shared_Response * const response)
{
    bool ret = false;
    if ((data->config != NULL) && (request != NULL) && (response != NULL))
    {
        uintptr_t location = 0U;
        if ((request->data.size < 1U) || (request->data.size > APP_SERVER_TRACE_MAX_WRITE_BYTES))
        {
            (void) strcpy(response->cause, "write size outside 1..8");
        }
        else if (!app_server_trace_private_resolve(data->config->writableRegions,
                                                   data->config->writableRegionCount,
                                                   request->address, request->data.size, &location))
        {
            (void) strcpy(response->cause, "write span not writable");
        }
        else
        {
            // Whole-or-prior for every firmware reader: the copy lands inside
            // one critical section.
            taskENTER_CRITICAL();
            (void) memcpy((void *) location, request->data.bytes, request->data.size);
            taskEXIT_CRITICAL();
            ret = true;
        }
    }
    return ret;
}

bool app_server_trace_peekLen(size_t * const dataLen)
{
    bool ret = false;
    if ((data->config != NULL) && (dataLen != NULL) &&
        (app_server_trace_private_ringUsed() > 0U))
    {
        // The used check read head: fence before reading the record bytes.
        APP_SERVER_TRACE_BARRIER_ACQUIRE();
        *dataLen = ((size_t) app_server_trace_private_ringPeek(0U)) |
                   (((size_t) app_server_trace_private_ringPeek(1U)) << 8U);
        ret = true;
    }
    return ret;
}

bool app_server_trace_pop(uint32_t * const tick,
                          uint8_t * const buffer, size_t bufferLen,
                          size_t * const dataLen)
{
    bool ret = false;
    if ((data->config != NULL) && (tick != NULL) && (buffer != NULL) && (dataLen != NULL) &&
        (app_server_trace_private_ringUsed() > 0U))
    {
        // The used check read head: fence before reading the record bytes.
        APP_SERVER_TRACE_BARRIER_ACQUIRE();
        const size_t len = ((size_t) app_server_trace_private_ringPeek(0U)) |
                           (((size_t) app_server_trace_private_ringPeek(1U)) << 8U);
        if (len <= bufferLen)
        {
            *tick = ((uint32_t) app_server_trace_private_ringPeek(2U)) |
                    (((uint32_t) app_server_trace_private_ringPeek(3U)) << 8U) |
                    (((uint32_t) app_server_trace_private_ringPeek(4U)) << 16U) |
                    (((uint32_t) app_server_trace_private_ringPeek(5U)) << 24U);
            for (uint32_t i = 0U; i < (uint32_t) len; i++)
            {
                buffer[i] = app_server_trace_private_ringPeek(6U + i);
            }
            *dataLen = len;

            const uint32_t capacity = app_server_trace_private_capacity();
            uint32_t newTail = data->tail +
                               (APP_SERVER_TRACE_RECORD_HEADER_BYTES +
                                APP_SERVER_TRACE_TICK_BYTES + (uint32_t) len);
            if (newTail >= capacity)
            {
                newTail -= capacity;
            }
            // Publish last: the record stays owned until fully copied out.
            APP_SERVER_TRACE_BARRIER_RELEASE();
            data->tail = newTail;
            ret = true;
        }
    }
    return ret;
}
