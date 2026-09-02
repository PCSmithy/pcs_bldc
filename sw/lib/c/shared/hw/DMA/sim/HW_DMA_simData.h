#pragma once

// Sim-internal driver state. Included by HW_DMA.c and its Unity suite only —
// it is the same object SIL reaches by DWARF, so injection and observation stay
// white-box on both surfaces and the driver carries no test-only API.

/* Includes */
#include "HW_DMA.h"

/* Defines */

// Largest mem-to-periph payload the capture buffer holds. A longer transfer
// still moves every byte through the caller's buffer; only the copy is clamped.
#define HW_DMA_SIM_MAX_BYTES  (256U)

/* Typedefs */

typedef struct
{
    HW_DMA_completeCallback_F callback;
    void * callbackContext;
    HW_DMA_status_E status;

    // In-flight transfer, settled by the pended completion interrupt.
    void *   memory;
    uint32_t numItems;
    bool     pending;

    // The bytes a mem-to-periph transfer handed the engine — the observation
    // point for what firmware put on the wire. No framework consumer yet.
    uint8_t lastMem[HW_DMA_SIM_MAX_BYTES];
    size_t  lastMemLen;

    // Fault knob, written by DWARF from SIL: the next completion lands ERROR.
    bool     forceError;
    uint32_t transferCount;
} HW_DMA_channelData_S;

typedef struct
{
    const HW_DMA_config_S * config;
    bool initialized;

    HW_DMA_channelData_S channels[HW_DMA_CHANNEL_COUNT];
} HW_DMA_data_S;

/* Public Data Declarations */

extern HW_DMA_data_S HW_DMA_data;
