# Firmware specs

`fw~` requirements: STM32G4 firmware behavior, written in C/C++ on
FreeRTOS. Each spec needs at least one `impl` tag (in C/C++ source) and at
least one `test` tag (unit and/or SIL).

## Topics

Sub-folders are created when a topic gets its first spec.

- `architecture/` — task structure, ISR design, memory layout, build
  configuration
- `foc/` — Park / Clarke transforms, PWM modulation strategies
  (SVPWM/SPWM), inner current control loop
- `estimation/` — Kalman observer, sensorless observer, parameter
  identification
- `motion/` — velocity loop, position loop, trajectory generator
- `drivers/` — STSPIN32G4 gate driver, AS5048 encoder, ADC current
  sensing, USB CDC stack
- `telemetry/` — USB streaming frame format, ring buffers, sample-rate
  control
- `safety/` — fault state machine, protection thresholds, recovery
  paths

## Tagging conventions in firmware code

```c
// [impl->fw~foc_park_transform~1]
void foc_park_transform(...) { ... }
```

```c
// [test->fw~foc_park_transform~1]
TEST(foc, park_transform_matches_reference) { ... }
```

A single piece of code can carry multiple tags if it implements multiple
specs (rare; usually a sign you should split the code).
