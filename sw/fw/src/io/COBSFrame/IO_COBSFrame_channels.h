#pragma once

/* Defines */

// Largest decoded frame payload any channel may declare; sizes the driver's
// static assembly/held-frame buffers. 448 clears the largest inbound
// envelope: a watch-capacity WatchRequest (32 x ~12 B entries + overhead).
#define IO_COBSFRAME_MAX_PAYLOAD  448U

/* Typedefs */

// Logical frame channels — one per framed protocol stream.
typedef enum
{
    IO_COBSFRAME_CHANNEL_CDC,   // protocol frames over the CDC serial channel
    IO_COBSFRAME_CHANNEL_COUNT,
} IO_COBSFrame_channel_E;
