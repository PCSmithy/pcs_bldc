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
