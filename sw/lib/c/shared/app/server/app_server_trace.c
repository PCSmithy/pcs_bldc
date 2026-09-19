/* Includes */
#include "app_server_trace.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* Defines */

// Ring record layout: [dataLen u16 LE][group u8][cycle u32 LE][data...]. The
// header bytes are ring bookkeeping only; they never ride the wire.

// Most records one millisecond can hold: 20 one-cycle plus one each of the
// 20- and 200-cycle groups. The u formula charges only CYCLE_BYTES of each.
#define APP_SERVER_TRACE_RECORDS_PER_MS_MAX (22U)

#define APP_SERVER_TRACE_MAX_RECORD_DATA_BYTES (sizeof(((trace_Samples *) 0)->data.bytes))
#define APP_SERVER_TRACE_MAX_WATCH_BYTES       (8U)
#define APP_SERVER_TRACE_MAX_READ_BYTES        (sizeof(((trace_ReadReply *) 0)->data.bytes))
#define APP_SERVER_TRACE_MAX_WRITE_BYTES       (sizeof(((trace_WriteRequest *) 0)->data.bytes))

// Period groups (fw~conn_trace_004).
#define APP_SERVER_TRACE_GROUP_COUNT (3U)

// Baseline message rate of fw~conn_trace_009: emission runs once a millisecond.
#define APP_SERVER_TRACE_EMISSIONS_PER_S (1000U)

// SPSC ring fences ordering the record's plain data against the volatile index
// publishes: ACQUIRE after reading the peer's index, RELEASE before publishing.
#define APP_SERVER_TRACE_BARRIER_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define APP_SERVER_TRACE_BARRIER_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#define APP_SERVER_TRACE_BARRIER_FULL()    __atomic_thread_fence(__ATOMIC_SEQ_CST)

// The board's storage macro must cover every record header a full millisecond
// can carry past the budget, plus the ring's one empty slot.
_Static_assert(APP_SERVER_TRACE_RING_OVERHEAD_BYTES ==
                   ((APP_SERVER_TRACE_RECORDS_PER_MS_MAX *
                     APP_SERVER_TRACE_RECORD_HEADER_BYTES) + 1U),
               "ring overhead mismatch");

/* Private Data Definitions */

// One period group: wire period, the stagger offset keeping groups out of one
// another's cycles (fw~conn_trace_004), and n_g / f_g for the admission formulas.
typedef struct
{
    uint32_t periodCycles;
    uint32_t offset;
    uint32_t recordsPerMs;
    uint32_t recordsPerS;
} app_server_trace_group_S;

static const app_server_trace_group_S app_server_trace_groups[APP_SERVER_TRACE_GROUP_COUNT] =
{
    { .periodCycles = 1U,   .offset = 0U, .recordsPerMs = 20U, .recordsPerS = 20000U },
    { .periodCycles = 20U,  .offset = 1U, .recordsPerMs = 1U,  .recordsPerS = 1000U  },
    { .periodCycles = 200U, .offset = 2U, .recordsPerMs = 1U,  .recordsPerS = 100U   },
};

// Where a group's entries sit in the list and how big its record is (S_g).
typedef struct
{
    uint16_t first;
    uint16_t count;
    uint16_t dataBytes;
} app_server_trace_groupState_S;

typedef struct
{
    const app_server_config_S * config;
    // Watch double buffer: the sampler streams `active` while decode fills
    // `staged`; admission swaps them behind the gate below, never half-committed.
    app_server_watch_S * active;
    app_server_watch_S * staged;
    app_server_trace_groupState_S groupState[APP_SERVER_TRACE_GROUP_COUNT];
    // The sampler's one gate. Zero stops it dead, so the task publishes zero
    // before touching anything the sampler reads and the new count last.
    volatile uint32_t activeCount;
    uint32_t stagedCount;
    bool stagedRejected;
    char stagedCause[sizeof(((shared_Response *) 0)->cause)];
    volatile uint32_t cycleIndex;
    // Sample ring: SPSC while streaming (sampler owns head, server owns tail);
    // cross-writer resets only happen with the gate closed.
    volatile uint32_t head;
    volatile uint32_t tail;
    // Sampler-owned: set by a skipped record, cleared once the ring has
    // drained to half, so a loss is one contiguous gap.
    bool overflowed;
} app_server_trace_data_S;

static app_server_trace_data_S app_server_trace_data;
static app_server_trace_data_S * const data = &app_server_trace_data;

/* Private Function Definitions */

// Physical ring size: the budget plus the header and empty-slot overhead, so a
// full millisecond of records at u == budget still fits.
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

// Open or close the sampler's gate. The fences keep the plain accesses either
// side of the publish from crossing it.
static void app_server_trace_private_publish(uint32_t count)
{
    APP_SERVER_TRACE_BARRIER_FULL();
    data->activeCount = count;
    APP_SERVER_TRACE_BARRIER_FULL();
}

// Close the gate, then restart the stream: the sampler is already quiet, so no
// capture can be in flight over the indices being reset.
static void app_server_trace_private_restart(void)
{
    app_server_trace_private_publish(0U);
    data->cycleIndex = 0U;
    data->head = 0U;
    data->tail = 0U;
    data->overflowed = false;
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
static void app_server_trace_private_stageEntry(uint32_t address, uint32_t size, uint32_t periodCycles)
{
    uintptr_t location = 0U;
    uint32_t group = APP_SERVER_TRACE_GROUP_COUNT;
    for (uint32_t g = 0U; g < APP_SERVER_TRACE_GROUP_COUNT; g++)
    {
        if (app_server_trace_groups[g].periodCycles == periodCycles)
        {
            group = g;
            break;
        }
    }

    if ((size < 1U) || (size > APP_SERVER_TRACE_MAX_WATCH_BYTES))
    {
        app_server_trace_private_stageReject("watch size outside 1..8");
    }
    else if (group >= APP_SERVER_TRACE_GROUP_COUNT)
    {
        app_server_trace_private_stageReject("watch period not 1/20/200 cycles");
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
            .group     = (uint8_t) group,
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
        app_server_trace_private_stageEntry(watch.address, watch.size, watch.period_cycles);
    }
    return ret;
}

// Stable-partition by group (watch-list order within a group is wire record
// order), then index each group's start, count, and record size S_g.
static void app_server_trace_private_index(app_server_watch_S * const list, uint32_t count,
                                           app_server_trace_groupState_S * const groupState)
{
    for (uint32_t i = 1U; i < count; i++)
    {
        const app_server_watch_S entry = list[i];
        uint32_t j = i;
        while ((j > 0U) && (list[j - 1U].group > entry.group))
        {
            list[j] = list[j - 1U];
            j--;
        }
        list[j] = entry;
    }

    uint32_t index = 0U;
    for (uint32_t g = 0U; g < APP_SERVER_TRACE_GROUP_COUNT; g++)
    {
        app_server_trace_groupState_S * const state = &groupState[g];
        state->first     = (uint16_t) index;
        state->count     = 0U;
        state->dataBytes = 0U;
        while ((index < count) && (((uint32_t) list[index].group) == g))
        {
            state->count++;
            state->dataBytes += list[index].sizeBytes;
            index++;
        }
    }
}

// The fw~conn_trace_002 formulas over an indexed list: widest record, u in
// bytes per ms, r in bytes per second (m_g floors, so r rounds down).
static void app_server_trace_private_usage(const app_server_trace_groupState_S * const groupState,
                                           uint32_t * const maxRecordBytes,
                                           uint32_t * const ramPerMs,
                                           uint32_t * const linkRate)
{
    uint32_t widest = 0U;
    uint32_t ram = 0U;
    uint32_t rate = 0U;
    for (uint32_t g = 0U; g < APP_SERVER_TRACE_GROUP_COUNT; g++)
    {
        const app_server_trace_group_S * const group = &app_server_trace_groups[g];
        const uint32_t recordBytes = groupState[g].dataBytes;
        if (groupState[g].count > 0U)
        {
            if (recordBytes > widest)
            {
                widest = recordBytes;
            }
            ram += group->recordsPerMs * (APP_SERVER_TRACE_CYCLE_BYTES + recordBytes);

            // m_g: one message per emission, unless the group's byte rate needs
            // more, and never more than one message per record.
            uint32_t messages = (group->recordsPerS * recordBytes) /
                                (uint32_t) APP_SERVER_TRACE_MAX_RECORD_DATA_BYTES;
            if (messages < APP_SERVER_TRACE_EMISSIONS_PER_S)
            {
                messages = APP_SERVER_TRACE_EMISSIONS_PER_S;
            }
            if (messages > group->recordsPerS)
            {
                messages = group->recordsPerS;
            }
            rate += (recordBytes * group->recordsPerS) +
                    (APP_SERVER_TRACE_WIRE_OVERHEAD_BYTES * messages);
        }
    }
    *maxRecordBytes = widest;
    *ramPerMs = ram;
    *linkRate = rate;
}

// [impl->fw~conn_trace_004~1] one group's entries captured as one coherent
// snapshot, or the whole record skipped when it does not fit.
static void app_server_trace_private_capture(uint32_t group, uint32_t cycle)
{
    const app_server_trace_groupState_S * const state = &data->groupState[group];
    const uint32_t dataLen = state->dataBytes;
    const uint32_t capacity = app_server_trace_private_capacity();
    const uint32_t needed = APP_SERVER_TRACE_RECORD_HEADER_BYTES +
                            APP_SERVER_TRACE_CYCLE_BYTES + dataLen;
    const uint32_t used = app_server_trace_private_ringUsed();
    const uint32_t freeBytes = (capacity - 1U) - used;
    // A record that does not fit is skipped whole; the cycle-index gap is the
    // host's drop signal. Skipping then holds until the ring has drained to
    // half: one record admitted per record drained would scatter the loss
    // as single-record gaps, each breaking a message batch, and the small
    // messages that follow cost the link more than the records they carry.
    if (data->overflowed && (used <= (capacity / 2U)))
    {
        data->overflowed = false;
    }
    if (needed > freeBytes)
    {
        data->overflowed = true;
    }
    if (!data->overflowed)
    {
        // The free-space check read tail: fence before reusing space the
        // consumer just released.
        APP_SERVER_TRACE_BARRIER_ACQUIRE();
        const uint8_t header[APP_SERVER_TRACE_RECORD_HEADER_BYTES] = {
            (uint8_t) (dataLen & 0xFFU),
            (uint8_t) ((dataLen >> 8U) & 0xFFU),
            (uint8_t) group,
        };
        const uint8_t cycleBytes[APP_SERVER_TRACE_CYCLE_BYTES] = {
            (uint8_t) (cycle & 0xFFU),
            (uint8_t) ((cycle >> 8U) & 0xFFU),
            (uint8_t) ((cycle >> 16U) & 0xFFU),
            (uint8_t) ((cycle >> 24U) & 0xFFU),
        };
        uint32_t index = data->head;
        index = app_server_trace_private_ringWrite(index, header, APP_SERVER_TRACE_RECORD_HEADER_BYTES);
        index = app_server_trace_private_ringWrite(index, cycleBytes, APP_SERVER_TRACE_CYCLE_BYTES);
        const app_server_watch_S * const list = data->active;
        for (uint32_t i = 0U; i < (uint32_t) state->count; i++)
        {
            const app_server_watch_S * const entry = &list[(uint32_t) state->first + i];
            index = app_server_trace_private_ringWrite(index,
                                                       (const uint8_t *) entry->location,
                                                       entry->sizeBytes);
        }
        // Publish last: the consumer never sees a partial record.
        APP_SERVER_TRACE_BARRIER_RELEASE();
        data->head = index;
    }
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
        data->stagedCount = 0U;
        data->stagedRejected = false;
        data->stagedCause[0] = '\0';
        (void) memset(data->groupState, 0, sizeof(data->groupState));
        app_server_trace_private_restart();
        ret = true;
    }
    return ret;
}

// [impl->fw~conn_trace_004~1]
void app_server_trace_sampleCycle(void)
{
    if (data->activeCount > 0U)
    {
        const uint32_t cycle = data->cycleIndex;
        data->cycleIndex = cycle + 1U;
        for (uint32_t g = 0U; g < APP_SERVER_TRACE_GROUP_COUNT; g++)
        {
            const app_server_trace_group_S * const group = &app_server_trace_groups[g];
            // Unsigned wrap keeps pre-offset cycles indivisible, so each group
            // first fires on its own offset; 2^32 cycles (~60 h) rolls it out of phase.
            if ((data->groupState[g].count > 0U) &&
                (((cycle - group->offset) % group->periodCycles) == 0U))
            {
                app_server_trace_private_capture(g, cycle);
            }
        }
    }
}

// [impl->fw~conn_trace_003~1]
void app_server_trace_clear(void)
{
    if (data->config != NULL)
    {
        app_server_trace_private_restart();
        (void) memset(data->groupState, 0, sizeof(data->groupState));
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
        app_server_trace_groupState_S staged[APP_SERVER_TRACE_GROUP_COUNT];
        app_server_trace_private_index(data->staged, data->stagedCount, staged);

        uint32_t maxRecordBytes = 0U;
        uint32_t ramPerMs = 0U;
        uint32_t linkRate = 0U;
        app_server_trace_private_usage(staged, &maxRecordBytes, &ramPerMs, &linkRate);

        if (data->stagedRejected)
        {
            (void) strcpy(response->cause, data->stagedCause);
        }
        else if (maxRecordBytes > APP_SERVER_TRACE_MAX_RECORD_DATA_BYTES)
        {
            (void) strcpy(response->cause, "exceeds Samples data capacity");
        }
        else if (ramPerMs > data->config->sampleRamBudgetBytes)
        {
            (void) strcpy(response->cause, "exceeds sample-RAM budget");
        }
        else if (linkRate > data->config->linkBudgetBytesPerS)
        {
            (void) strcpy(response->cause, "exceeds link budget");
        }
        else
        {
            // Commit behind a closed gate: the sampler stops, the halves swap,
            // and the new list is published whole.
            app_server_trace_private_restart();
            app_server_watch_S * const previousActive = data->active;
            data->active = data->staged;
            data->staged = previousActive;
            (void) memcpy(data->groupState, staged, sizeof(data->groupState));
            app_server_trace_private_publish(data->stagedCount);
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
        uint32_t maxRecordBytes = 0U;
        uint32_t ramPerMs = 0U;
        uint32_t linkRate = 0U;
        app_server_trace_private_usage(data->groupState, &maxRecordBytes, &ramPerMs, &linkRate);
        status->ram_budget_bytes_per_ms = data->config->sampleRamBudgetBytes;
        status->ram_usage_bytes_per_ms  = ramPerMs;
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
            // The critical section holds off other tasks; the board hook masks
            // the cycle callback, which runs above the FreeRTOS syscall priority
            // and ignores BASEPRI.
            const bool maskable = (data->config->setSamplerMasked != NULL);
            taskENTER_CRITICAL();
            if (maskable)
            {
                data->config->setSamplerMasked(true);
            }
            (void) memcpy((void *) location, request->data.bytes, request->data.size);
            if (maskable)
            {
                data->config->setSamplerMasked(false);
            }
            taskEXIT_CRITICAL();
            ret = true;
        }
    }
    return ret;
}

uint32_t app_server_trace_groupPeriodCycles(uint32_t group)
{
    return ((group < APP_SERVER_TRACE_GROUP_COUNT)) ? app_server_trace_groups[group].periodCycles : 0U;
}

uint32_t app_server_trace_bufferedBytes(void)
{
    return (data->config != NULL) ? app_server_trace_private_ringUsed() : 0U;
}

bool app_server_trace_peek(uint32_t * const group, uint32_t * const cycle, size_t * const dataLen)
{
    bool ret = false;
    if ((data->config != NULL) && (group != NULL) && (cycle != NULL) && (dataLen != NULL) &&
        (app_server_trace_private_ringUsed() > 0U))
    {
        // The used check read head: fence before reading the record bytes.
        APP_SERVER_TRACE_BARRIER_ACQUIRE();
        *dataLen = ((size_t) app_server_trace_private_ringPeek(0U)) |
                   (((size_t) app_server_trace_private_ringPeek(1U)) << 8U);
        *group = (uint32_t) app_server_trace_private_ringPeek(2U);
        *cycle = ((uint32_t) app_server_trace_private_ringPeek(3U)) |
                 (((uint32_t) app_server_trace_private_ringPeek(4U)) << 8U) |
                 (((uint32_t) app_server_trace_private_ringPeek(5U)) << 16U) |
                 (((uint32_t) app_server_trace_private_ringPeek(6U)) << 24U);
        ret = true;
    }
    return ret;
}

bool app_server_trace_pop(uint8_t * const buffer, size_t bufferLen)
{
    bool ret = false;
    if ((data->config != NULL) && (buffer != NULL) &&
        (app_server_trace_private_ringUsed() > 0U))
    {
        // The used check read head: fence before reading the record bytes.
        APP_SERVER_TRACE_BARRIER_ACQUIRE();
        const size_t len = ((size_t) app_server_trace_private_ringPeek(0U)) |
                           (((size_t) app_server_trace_private_ringPeek(1U)) << 8U);
        if (len <= bufferLen)
        {
            const uint32_t dataOffset = APP_SERVER_TRACE_RECORD_HEADER_BYTES +
                                        APP_SERVER_TRACE_CYCLE_BYTES;
            const uint32_t capacity = app_server_trace_private_capacity();
            // The data lies in at most two runs around the wrap: copy them
            // whole rather than a byte at a time through the peek.
            uint32_t start = data->tail + dataOffset;
            if (start >= capacity)
            {
                start -= capacity;
            }
            const uint32_t firstRun = ((start + (uint32_t) len) > capacity) ? (capacity - start) : (uint32_t) len;
            (void) memcpy(buffer, &data->config->sampleStorage[start], firstRun);
            if (firstRun < (uint32_t) len)
            {
                (void) memcpy(&buffer[firstRun], data->config->sampleStorage, (uint32_t) len - firstRun);
            }

            uint32_t newTail = data->tail + dataOffset + (uint32_t) len;
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
