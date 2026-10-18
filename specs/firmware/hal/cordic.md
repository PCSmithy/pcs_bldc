---
status: draft
tags: [firmware, hal, cordic, driver]
---

# CORDIC hardware abstraction layer

The `HW_CORDIC` driver is the hw-layer abstraction over the MCU's
trigonometric coprocessor. It returns the sine and cosine of an angle in
one call, for the per-PWM-cycle transforms that otherwise spend their
budget in library trigonometry. The sim computes the same values with the
C library.

See also: [[overview]] (sys~arch_005~1), [[control-loop-timing]]
(sys~mc_006~1, the budget the coprocessor serves), [[modulation]]
(fw~mc_013~1, the transforms that consume the pair).

## Driver configuration and lifecycle

### Initialization and configuration validation
`fw~hal_cordic_001~1`

The driver shall validate the supplied configuration and leave the
coprocessor clocked and configured for a sine-and-cosine evaluation,
returning false when the config pointer is NULL or the configured
precision is outside the coprocessor's range.

Acceptance:
- A valid configuration returns true and a subsequent evaluation succeeds.
- A NULL config or an out-of-range precision returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Evaluation

### Sine and cosine of an angle
`fw~hal_cordic_002~1`

After initialization, the driver shall return the sine and cosine of an
angle in radians of magnitude up to 4 pi, each within 1e-5 of the
mathematical value, and shall return false with the outputs untouched
before initialization or when an output pointer is NULL.

Acceptance:
- For angles 0, pi/6, pi/2, pi, -pi/6, 2 pi + pi/6, and -4 pi + pi/2, the
  returned pair is within 1e-5 of the reference values.
- A NULL output pointer returns false and leaves the other output as it was.

Covers:
- sys~arch_005~1

Needs: impl, test
