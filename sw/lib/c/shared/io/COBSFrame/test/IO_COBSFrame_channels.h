#pragma once

// Test-local channel seam for the IO_COBSFrame unit suite (stands in for the
// project's IO_COBSFrame_channels.h).

/* Defines */

// Largest decoded frame payload any channel may declare; sizes the driver's
// static assembly/held-frame buffers.
#define IO_COBSFRAME_MAX_PAYLOAD  256U

/* Typedefs */

typedef enum
{
    IO_COBSFRAME_CHANNEL_CDC,
    IO_COBSFRAME_CHANNEL_COUNT,
} IO_COBSFrame_channel_E;
