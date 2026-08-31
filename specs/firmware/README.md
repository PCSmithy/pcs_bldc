# Firmware specs

`fw~` requirements: STM32G4 firmware behavior, written in C/C++ on
FreeRTOS. Each spec needs at least one `impl` tag (in C/C++ source) and at
least one `test` tag (unit and/or SIL).

## Topics

Sub-folders are created when a topic gets its first spec.

- `architecture/` — task structure, ISR design, memory layout, build
  configuration
- `hal/` — hardware abstraction layer (hw-layer peripheral drivers: SPI,
  ADC, GPIO, DMA, timers, USB, op-amps, I2C). Uses per-peripheral sub-topic
  IDs, e.g. `fw~hal_spi_001~1`, `fw~hal_i2c_001~1`, `fw~hal_adc_001~1`,
  `fw~hal_usb_001~1`, `fw~hal_opamp_001~1`
- `io/` — generic, domain-independent io-layer infrastructure drivers (device
  abstractions layered over HAL buses). Per-bus sub-topic IDs, e.g.
  `fw~io_i2c_001~1` (IO_i2c generic per-device I2C driver over HW_I2C),
  `fw~io_bridge_001~1` (IO_bridge 3-phase motor-bridge driver over HW_TIM)
- `pd/` — USB-PD sink status: register decode and runtime for the CYPD3177
  controller, e.g. `fw~pd_001~1` (lib_CYPD3177 decode library)
- `mc/` — motor control, e.g. `fw~mc_001~1` (dev_gateDriver STSPIN32G4
  gate-driver management); also FOC, motion, trajectory tracking
- `est/` — estimation. Per-area sub-topic IDs, e.g. `fw~est_encoder_001~1`
  (AS5048 magnetic encoder driver), `fw~est_velocity_001~1` (encoder-derived
  velocity estimate); also the Kalman observer, sensorless observer, and
  parameter identification
- `obs/` — observability and on-device status indication. Per-area sub-topic
  IDs, e.g. `fw~obs_led_001~1` (SK6805 RGB LED string driver),
  `fw~obs_identity_*` (build identity), `fw~obs_status_001~1` (10 Hz status
  publication), `fw~obs_log_*` (printf log stream)
- `ui/` — on-device user-interface input drivers. Per-area sub-topic IDs, e.g.
  `fw~ui_switch_001~1` (dev_switch debounced button/switch driver)
- `conn/` — device↔app connectivity (the firmware side). Per-area sub-topic
  IDs: `fw~conn_serial_*` (IO_serial byte-stream transport over USB CDC),
  `fw~conn_proto_*` (protobuf schema + IO_COBSFrame framing),
  `fw~conn_server_*` (app_server request dispatch), `fw~conn_trace_*`
  (app_server trace services: watch streaming, memory read/write,
  trace capability report)
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
