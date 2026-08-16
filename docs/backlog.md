# Firmware backlog

Deferred firmware tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. SIL-specific items live in `sil/backlog.md`.

## HW_I2C stuck-bus recovery

**When:** when a bus that dies mid-operation must recover without a reboot —
at the latest, before motor operation depends on live CYPD3177 or gate-driver
status. No urgency while the buses either work (I2C3) or are down for
hardware reasons no software can fix (I2C1 until the C28 rework).

**What it is:** the HW_I2C driver handles per-transfer failure (NACK, error
IRQ, timeout + abort) but has no recovery for a bus whose wires are stuck: a
slave holding SDA low mid-bit, or SCL crippled so transfers can't clock. In
that state `HAL_I2C_Master_Abort_IT` can't complete (the abort itself needs a
working SCL), the HAL handle latches in ABORT/BUSY, and every subsequent
transfer on that bus returns `HAL_BUSY` immediately — the bus is dead until
reset.

**Observed on the bench (2026-07-05):** with C28 loading I2C1's SCL, the
first CYPD3177 fetch after boot burns its timeout + abort drain, the handle
wedges, and all later fetches fail instantly (`task200ms_us` ~300 µs steady
state instead of 7 × ~2 ms timeouts). Harmless today — the bus is physically
unusable anyway and the state clears on reset — but it demonstrates the
failure mode end to end.

**The fix, when picked up:**

1. Detect the wedge in `HW_I2C_private_transfer`: HAL start call returns
   `HAL_BUSY` while the driver believes no transfer is in flight (or N
   consecutive instant failures on a bus).
2. Standard 9-clock recovery: reconfigure SCL as a GPIO, clock up to 9 pulses
   until the slave releases SDA, generate a manual STOP (SDA low→high while
   SCL high).
3. `HAL_I2C_DeInit` / `HAL_I2C_Init` (+ analog/digital filter reconfig, as in
   `HW_I2C_init`) to clear the latched HAL/peripheral state, then restore the
   pins to AF open-drain.
4. Rate-limit recovery attempts (e.g. once per second) so a hard-failed bus
   doesn't turn the 200 ms task into a GPIO-thrash loop.

**Where:** `sw/lib/c/shared/hw/I2C/stm32g4/HW_I2C.c` (detection + reinit) —
the pin bit-bang needs the bus's GPIO port/pin, which the driver doesn't
currently know; either extend `HW_I2C_busConfig_S` with the SCL/SDA pin
identities or route the recovery through HW_GPIO. Sim side: model a stuck bus
(`sim/HW_I2C.c` stall is per-transfer today, not sticky-with-recovery) so the
recovery path is unit-testable. Spec: extend `specs/firmware/hal/i2c.md`
(likely a new `fw~hal_i2c_005`) before implementing.

**Origin:** cypd3177-i2c branch review finding (2026-07-04), confirmed by the
C28 bench behavior above.

## App-initiated DFU firmware update over USB

**When:** after the basic desktop app is up (it owns the orchestration and
the UX). No firmware groundwork blocks on it — the protocol seams it needs
already exist.

**What it is:** update the firmware from the desktop app with no debugger
attached. The G431's ROM system bootloader (AN2606) already speaks standard
USB DFU (DfuSe, AN3156) and enumerates as its own device (`0483:DF11`), so
the image bytes never travel over our protocol: the app's framed protocol
only *triggers* the transition and *verifies* the result. No resident custom
bootloader — the ROM is flash-free and unbrickable, and the 128 KB
single-bank part can't do A/B anyway.

**The design, when picked up:**

1. Schema: `EnterBootloaderRequest` in shared.proto's framework range
   (field 7, an action — answered with `Response` per the reply
   convention).
2. Server core: on accept, arm a ~100 ms countdown so the `Response`
   drains out the USB FIFO before the reset, then invoke a new third board
   hook `bool (*enterBootloader)(void)` in `app_server_config_S` (NULL →
   rejected "unsupported").
3. Board hook (the G4 mechanism): write a magic word to a `.noinit` RAM
   location, `NVIC_SystemReset()`; check the magic at the very top of
   `Reset_Handler` (before .data/.bss init and clocks), clear it, set MSP
   from `0x1FFF0000`, jump to system memory. Never jump to ROM from a
   running app with USB/PLLs live.
4. App orchestration: send request → close port → hotplug-watch CDC
   disappear + DFU `0483:DF11` appear → DfuSe erase/download (dfu-core
   crate or dfu-util/CubeProgrammer) → DFU "leave" → CDC re-enumerates →
   reconnect → `IdentityRequest` and assert the reported build identity
   matches the flashed image (the existing identity service is the
   verification step; the DWARF gate re-arms automatically).
5. Specs first: a `sys~` update spec (`Needs: fw, app, test`), fw~ specs
   for the service + reset path, app~ specs for the orchestration state
   machine.

**Known warts:** Windows binds no driver to `0483:DF11` — first use needs
WinUSB via Zadig or the CubeProgrammer driver; the app should detect and
walk the user through it (macOS needs nothing). Interrupted flash: the G4
empty-check reboots into the ROM bootloader when bank start is erased, so
retry usually just works; confirm BOOT0 is physically reachable on the
board as the unconditional escape hatch before shipping the feature.

**Origin:** serial-protocol branch design discussion (2026-08-15), as a
thought experiment validating the app_server board-hook extension pattern.

## IO_voltageMonitor: logical measurement channels over HW_ADC

**When:** before more consumers need engineering-unit measurements — the
signal-trace and control work will want the same bus/rail/phase values the
telemetry hook reads today.

**What it is:** analog measurements have no owning module. The
app_server telemetry hook reads raw pin volts (`HW_ADC_getVolts`) and
applies the sense-front-end scaling inline (`APP_SERVER_VBUS_*` defines),
and the retired Teleplot scaffolding did the same for phase
currents/voltages and rails. An io-layer `IO_voltageMonitor` would map
logical channels (VBUS_V, VBUS_I, RAIL_5V0, phase senses, ...) to
`(ADC channel, input, offset, scale, unit)` channel config, returning
engineering units (volts and non-volt units like amps) from one place.

**Where:** `sw/lib/c/shared/io/voltageMonitor/` + `sw/fw/src/io/` channel
config carrying the board's divider/shunt/gain constants; spec under
`specs/firmware/io/`. The app_server hook and IO_bridge's current-sense
scaling become consumers.

**Origin:** TODO in `app_server_config.c` (2026-08-16), serial-protocol
branch.

## Sim USB: model the unconfigured (pre-enumeration) state

**When:** with the SIL transport work, or the next time USB lifecycle
behavior matters in a test.

**What it is:** the sim `HW_USB` (loopback) has no notion of the device
being unconfigured: reads/writes touch its buffers from tick 0. Real
TinyUSB is unsafe there — its CDC FIFO paths claim endpoints unguarded,
and before the host configures the device the CDC endpoints are address
0, so a read during enumeration queues transfers on the control endpoint
and trips `TU_ASSERT(ep_status.busy == 0)` in `usbd_edpt_xfer`. This
exact failure shipped on the serial-protocol branch (server task pumping
`tud_cdc_read` every 1 ms from boot → "Device Descriptor Request Failed"
on the bench) and no native test could see it.

**The fix, when picked up:** give the sim a `configured` flag distinct
from `connected` (mounted vs port-open, mirroring `tud_ready()` vs
`tud_cdc_connected()`): FIFO paths return 0 / accept nothing while
unconfigured, a `HW_USB_sim_setConfigured()` hook drives it, and reset
starts unconfigured. Then a unit test asserting "no FIFO traffic before
configuration" turns this class of bench bug into a red test. Keep the
guard in `stm32g4/HW_USB.c` regardless — defense in depth at the layer
that owns the constraint.

**Origin:** bench enumeration failure on the serial-protocol branch
(2026-08-16), root-caused to the unguarded TinyUSB CDC read path.
