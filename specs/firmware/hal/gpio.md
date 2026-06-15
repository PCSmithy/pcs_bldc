---
status: draft
tags: [firmware, hal, gpio, driver]
---

# GPIO hardware abstraction layer

The `HW_GPIO` driver is the generic, reusable hw-layer abstraction over the
MCU's general-purpose I/O pins. It presents a target-independent digital-I/O
API addressed by port and pin, usable by any consumer through a single header
that is identical across the embedded (STM32G4) and simulation (SIM) build
targets.

Configuration is organized by port: each port declares the pins it owns, and
each pin declares its mode (input, output, or interrupt input) and the
settings that mode requires.

See also: [[overview]] (sys~arch_005~1), [[spi]] (HW_SPI consumes HW_GPIO for
chip-select).

## Configuration and lifecycle

### GPIO initialization and configuration validation
`fw~hal_gpio_001~1`

The GPIO HW driver shall validate the supplied configuration before use and
shall apply it to every declared pin, returning false when the configuration
is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer is NULL. |
| Pin | Any declared pin is invalid per `fw~hal_gpio_002~1`. |

Acceptance:
- A valid config applies to all declared pins and returns true.
- Each rejection condition above returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Pin configuration validity
`fw~hal_gpio_002~1`

A pin's configuration shall be valid only when its pin selection and mode are
each well-formed:

| Field | Valid when |
|-------|-----------|
| Pins | Selects at least one GPIO line and no undefined lines. |
| Mode | Is input, output, or interrupt input. |

Acceptance:
- A config whose pin selection includes an undefined line is invalid.
- A config with an empty pin selection is invalid.
- A config with an unsupported mode is invalid.
- A config with a non-empty selection of defined lines and a supported mode is
  valid.

Covers:
- sys~arch_005~1

Needs: impl, test

## Pin I/O

### Output pin write
`fw~hal_gpio_003~1`

The driver shall drive a pin configured as an output to a commanded logical
level, high or low.

Acceptance:
- Writing high then low to an output pin leaves it at each commanded level in
  turn, observable on the addressed pin.
- A write addressing an out-of-range port is a no-op.

Covers:
- sys~arch_005~1

Needs: impl, test

### Input pin read
`fw~hal_gpio_004~1`

The driver shall report the present logical level of a pin configured as an
input.

Acceptance:
- Reading an input pin returns its present level, high or low.
- With a pin's input level injected through the SIL control API, a read
  returns the injected level.

Covers:
- sys~arch_005~1

Needs: impl, test

## Interrupts

### Pin-change interrupt callbacks
`fw~hal_gpio_005~1`

For a pin configured as an interrupt input, the driver shall invoke a
caller-registered callback once on each configured signal edge.

Acceptance:
- After a callback is registered for an interrupt pin, each edge event invokes
  that callback exactly once.
- An edge injected through the SIL control API invokes the registered
  callback.
- An edge on a pin with no registered callback invokes nothing.

Covers:
- sys~arch_005~1

Needs: impl, test
