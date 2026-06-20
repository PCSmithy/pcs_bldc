# Firmware specs

`fw~` requirements: STM32G4 firmware behavior, written in C/C++ on
FreeRTOS. Each spec needs at least one `impl` tag (in C/C++ source) and at
least one `test` tag (unit and/or SIL).

## Topics

Sub-folders are created when a topic gets its first spec.

- `architecture/` — task structure, ISR design, memory layout, build
  configuration
- `hal/` — hardware abstraction layer (hw-layer peripheral drivers: SPI,
  ADC, GPIO, DMA, timers, USB). Uses per-peripheral sub-topic IDs, e.g.
  `fw~hal_spi_001~1`, `fw~hal_adc_001~1`, `fw~hal_usb_001~1`
- `foc/` — Park / Clarke transforms, PWM modulation strategies
  (SVPWM/SPWM), inner current control loop
- `est/` — estimation. Per-area sub-topic IDs, e.g. `fw~est_encoder_001~1`
  (AS5048 magnetic encoder driver); also the Kalman observer, sensorless
  observer, and parameter identification
- `obs/` — observability and on-device status indication. Per-area sub-topic
  IDs, e.g. `fw~obs_led_001~1` (SK6805 RGB LED string driver); also telemetry
  and logging
- `conn/` — device↔app connectivity (the firmware side). Per-area sub-topic
  IDs, e.g. `fw~conn_serial_001~1` (IO_serial byte-stream transport over USB
  CDC); also the future framing protocol
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
