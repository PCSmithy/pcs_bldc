#pragma once

/* Defines */

// Largest decoded frame payload any channel may declare; sizes the driver's
// static assembly/held-frame buffers.
#define IO_COBSFRAME_MAX_PAYLOAD  256U

/* Typedefs */

// Logical frame channels — one per framed protocol stream.
typedef enum
{
    IO_COBSFRAME_CHANNEL_CDC,   // protocol frames over the CDC serial channel
    IO_COBSFRAME_CHANNEL_COUNT,
} IO_COBSFrame_channel_E;
