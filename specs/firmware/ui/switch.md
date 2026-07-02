---
status: draft
tags: [firmware, ui, switch, driver]
---

# Debounced switch driver

The `dev_switch` driver is the dev-layer abstraction for debounced on/off
controls. It presents each logical switch channel's state through a uniform API
— unknown, inactive, or active — regardless of source: a hardware digital input
or a caller-supplied state provider (a serial command, another driver, or a
thresholded sensor routed in as a virtual switch). Consumers read the debounced
state without knowing where it originates.

See also: [[overview]] (sys~arch_001~1), [[gpio]] (HW_GPIO supplies the cached
input snapshot).

## Configuration and lifecycle

### Switch configuration and initialization
`fw~ui_switch_001~1`

The driver shall validate the supplied configuration and initialize every
configured channel to the unknown state, returning false if the configuration
is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer is NULL. |
| Hardware-input channel | Its GPIO port is out of range. |
| State-provider channel | Its state-provider callback is NULL. |
| Channel | Its source type is not a supported type. |

Acceptance:
- A valid configuration initializes every channel to the unknown state and
  returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_001~1

Needs: impl, test

## Debounced state

### Debounced channel state
`fw~ui_switch_002~1`

The driver shall present each channel's state as unknown, inactive, or active,
adopting a new state only after its source has held that state continuously for
the channel's configured debounce duration (elapsed wall-clock time). On each
periodic update the driver reads every channel's source to a raw state:

| Source type | Raw state |
|-------------|-----------|
| Hardware input | Active when the GPIO input reads the channel's configured active level, otherwise inactive. |
| State provider | The state its callback returns. |

Acceptance:
- An unsettled channel reports unknown; once its source has held one state for
  the debounce duration, the channel reports that state.
- A source change shorter than the debounce duration leaves the reported state
  unchanged.
- Entering and leaving the active state each require the full debounce duration.
- Channels debounce independently: settling one does not change another.
- Querying an out-of-range or uninitialized channel reports the unknown state.
- `isActive` reports true only when a channel's debounced state is active.

Covers:
- sys~arch_001~1

Needs: impl, test
