# Milestone — CYPD3177 USB-PD sink telemetry over I2C

**Branch:** `cypd3177-i2c` (off `main`; merges back as one functional unit)
**Status:** planning
**Introduces:** the `i2c` peripheral at the HAL, and the `pd` (power-delivery)
topic in the spec tree (first `sys~pd` / `fw~pd` specs).

## 1. Goal

The board's main motor power arrives through the **CYPD3177** USB-PD sink
controller. Its PD contract (voltage/current) is set by hardware straps, but the
chip also exposes an **I2C host interface (HPI)** for reading live status. This
milestone stands up the firmware to **read the CYPD3177's negotiated PD contract
and bus status over I2C** and expose it to the rest of the firmware — a
read-only telemetry capability, standalone-first, SIL-testable.

Concretely, when complete the firmware can answer: *is an explicit PD contract
in effect, at what negotiated voltage and current, and what is the live VBUS?*

## 2. Hardware facts (established by research)

### 2.1 I2C / HPI transaction model

| Fact | Value | Notes |
|------|-------|-------|
| I2C slave address | **0x08** (7-bit) | Fixed; no address strap. |
| Register offset | **16-bit, little-endian** | LSB transmitted first. This is HPIv1. |
| Read framing | write 2 offset bytes → **repeated-START** → read N | Device NAKs a read with no preceding offset write. Maps to `HAL_I2C_Mem_Read(..., I2C_MEMADD_SIZE_16BIT, ...)`. |
| Multi-byte data | **little-endian** | LSB at lowest byte. |
| Clock stretching | **required** | STM32 I2C supports this natively (do not set NOSTRETCH). |
| Interrupt line | **HPI_INT**, active-low, open-drain | Asserted only for responses/async events; status reads never assert it. |

### 2.2 Registers this milestone reads

| Name | Offset | Sz | Meaning / decode |
|------|--------|----|------------------|
| DEVICE_MODE | `0x0000` | 1 | "Alive" probe. **Real CYPD3177 silicon returns `0x95`** (Infineon-confirmed spec erratum) — treat as alive/nonzero, don't hard-match. |
| SILICON_ID | `0x0002` | 2 | Read from live part; do not assume the BCR-PLUS/LITE value. |
| PD_STATUS | `0x1008` | 4 | **bit10 = explicit contract present** ("am I negotiated?"). |
| TYPE_C_STATUS | `0x100C` | 4 | partner connected, CC polarity, Rp current advertised. |
| BUS_VOLTAGE | `0x100D` | 1 | **Live VBUS, 100 mV/LSB** (e.g. 0xC8 = 20.0 V). |
| CURRENT_PDO | `0x1010` | 4 | Source PDO in force. Fixed-supply: **voltage = bits[19:10] × 50 mV**, max current = bits[9:0] × 10 mA. → **negotiated voltage**. |
| CURRENT_RDO | `0x1014` | 4 | RDO the BCR sent. **op current = bits[19:10] × 10 mA**, obj-pos = bits[30:28]. → **negotiated current**. |

Interrupt/config registers (INTERRUPT `0x0006` W1C, DEV_RESPONSE `0x007E`,
PD_RESPONSE `0x1400`, EVENT_MASK `0x1024`, SELECT_SINK_PDO `0x1005`, RESET
`0x0008`) are **documented but out of scope** for this read-only milestone (see §7).

### 2.3 Verify-on-hardware flags

The authoritative register map comes from the Infineon BCR-PLUS/LITE HPI spec
(002-33575) cross-checked against a hardware-tested CYPD3177 driver; the
CYPD3177-specific HPI spec (v01_00) is login-gated. Confirm on real silicon:
DEVICE_MODE value (`0x95`), SILICON_ID value, EVENT_STATUS offset (`0x1034` vs
`0x1044` across the BCR family), and whether BUS_CURRENT (`0x1058`) is
implemented on this die. None block the read-only telemetry path above.

### 2.4 Board wiring (confirmed from schematic)

The MCU is the **STSPIN32G4** (ref U8) — the integrated gate driver with an
embedded STM32G431 core. Its package bonds out only a subset of pins; **PC8/PC9
do not exist on this part, so I2C3 is physically unavailable.** The CYPD3177 is
ref U4.

| CYPD3177 pin | Net | MCU pin | Function |
|--------------|-----|---------|----------|
| HPI_SDA (12) | `I2C_SDA` | **PB7** | I2C1_SDA (AF4) |
| HPI_SCL (13) | `I2C_SCL` | **PB8** | I2C1_SCL (AF4) — **also BOOT0** |
| HPI_INT (7) | `PD_INT` | **PC13** | GPIO, EXTI13-capable |
| GPIO_1 (8) | `PD_GPIO` | **PC14** | GPIO, EXTI14-capable |

- **Instance: I2C1** (PB7/PB8). Dedicated point-to-point bus — the CYPD3177 is
  the only device on it (the board's BNO055 IMU is on separate nets).
- **Pull-ups (all to 3V3):** SDA `R32` 5.1 kΩ, SCL `R46` 5.1 kΩ, INT `R16`
  2.4 kΩ. HPI_INT is active-low with the external pull-up present — no MCU
  internal pull-up needed.
- **Gotcha: PB8 = SCL doubles as BOOT0**, and the SCL pull-up is tied into the
  BOOT0 switch network. Be aware of the boot-strapping interaction when
  driving/holding SCL.
- Address `0x08` is fixed by the device (no strap pin in the schematic).
- **No MSP work needed** — HAL brings up the pins/clock inside `HAL_I2C_Init()`;
  the generated MSP already covers I2C1 on PB7/PB8.

## 3. Architecture — how it maps onto this codebase

Four layers, each target-swappable and independently testable:

```
  app / periodic telemetry
        │
   dev_CYPD3177 ....... runtime logic: fetch status, hold state, expose accessors
        │     \
        │      └── lib_CYPD3177 ... pure static reference: register map + decode
        │                            helpers (NO I2C calls)
   IO_i2c ............. GENERIC per-device I2C driver. "read register R from
        │               device X" → a transaction on that device's bus.
        │               Channelized per logical device (address + which bus +
        │               register-offset width). HW-agnostic; reusable by any
        │               I2C device.
   HW_I2C ............. STM32 HAL wrapper presenting a standardized, target-
        │               independent bus API. Owns the logical BUS (peripheral +
        │               shared bus params: clock/timing, transfer mode). A bus
        │               may carry multiple devices. Channelized per bus.
   STM32G4 HAL (I2C1)   [native SIL target: a pure-C HW_I2C, no HAL references]
```

- **`HW_I2C`** (hw) — a thin STM32-HAL wrapper presenting a standardized,
  target-independent bus API, so a `native` HW_I2C for SIL can be built with
  **zero HAL references**. **Owns the "Bus":** a bus may carry more than one
  device, and all devices on a bus share bus parameters (clock/timing, transfer
  mode). Channelized by **bus** (`HW_I2C_BUS_1` = I2C1, ...). Primitives take
  (bus, 7-bit device address): transmit / receive, and a memory/register
  transaction (write offset → repeated-START → read/write N, via
  `HAL_I2C_Mem_*`) so register access is one atomic bus transaction.
- **`IO_i2c`** (io) — a **generic** I2C device driver any device can use.
  Channelized **per logical device** (`IO_I2C_DEVICE_*`), independent of HW
  peripheral enumeration. Each device config maps a logical device →
  (HW_I2C bus, device address, register-offset width/endianness), and the driver
  maps a logical operation ("read register 0xA of device X, N bytes") onto the
  matching HW_I2C bus transaction. This is the reusable home the empty
  `sw/lib/c/shared/io/i2c/` stub was reserved for.
- **`lib_CYPD3177`** (cross-cutting lib) — the *static reference*: register
  offsets, field masks, response codes, the `0x08` address, plus **pure decode
  functions** (PDO→mV, RDO→mA, PD_STATUS→contract flags, BUS_VOLTAGE→mV). Zero
  bus calls; unit-testable in isolation with golden vectors.
- **`dev_CYPD3177`** (dev) — *runtime logic*: fetches the §2.2 registers through
  its `IO_i2c` device channel, decodes with `lib_CYPD3177`, holds per-channel
  status, exposes semantic accessors (`isContractActive`, `negotiatedVoltage_mV`,
  `negotiatedCurrent_mA`, `busVoltage_mV`, device-alive/validity). Native-
  testable against a mock/sim `IO_i2c` returning canned register bytes.

Generic `IO_i2c` (device abstraction) over bus-owning `HW_I2C` departs
deliberately from the device-specific `IO_AS5048` precedent: I2C
register-transaction access is genuinely generic and worth centralizing once,
whereas SPI framing is per-device.

## 4. Design decisions (confirmed)

### D1 — Shape of the "IO layer" of the I2C driver → **generic `IO_i2c` over a bus-owning `HW_I2C`**

- **`HW_I2C` owns the Bus.** Thin STM32-HAL wrapper with a standardized API so a
  native SIL `HW_I2C` carries no HAL references. A bus may hold multiple devices;
  all devices on a bus share bus parameters (clock, timing, transfer mode).
  Channelized per bus.
- **`IO_i2c` is a generic per-device driver.** Channelized per logical device
  (not tied to HW peripheral enumeration); maps "read register R from device X"
  to a transaction on that device's `HW_I2C` bus. Reusable by any I2C device;
  lives in the `io/i2c` stub.
- `lib_CYPD3177` + `dev_CYPD3177` sit on top of `IO_i2c`.

### D2 — Milestone scope → **(A) read-only telemetry**

**Core requirement: diagnostic visibility into the chip.** The §2.2 registers
only (all reads): negotiated V/I, contract-active, live VBUS, Type-C status,
device-alive probe. No writes, no interrupt line. Interrupt servicing (B) and
configuration/commands (C) are noted future additions, not in this milestone
(§7).

## 5. Spec plan (OFT IDs)

New system anchor (introduces the `pd` topic):

- **`sys~pd_001~1`** — *USB-PD sink power monitoring.* The firmware shall read
  the CYPD3177's negotiated PD contract and bus status over I2C and expose it to
  firmware consumers. Derives from the README power/platform goal. `Needs: fw, test`.

Per component (numbers are the plan; allocate with `tools/next-spec-id.py`):

| Component | Specs (topic) | Covers |
|-----------|---------------|--------|
| `HW_I2C` (bus) | `fw~hal_i2c_001` init/validate (bus config) · `_002` bus + device-address transfers (transmit/receive) · `_003` blocking transfer w/ timeout · `_004` register/memory read-write (offset width, repeated-start) | `sys~arch_005~1` |
| `IO_i2c` (generic device) | device-channelized register read/write mapping a logical device→(bus, addr, offset width). **ID space / parent TBD** — either continue `fw~hal_i2c_*`, or a new `io`/`hal_i2c`-io number space; parent likely `sys~arch_005~1` (reusable target-independent abstraction). Settle in `/pcs_spec`. | `sys~arch_005~1` (tent.) |
| `lib_CYPD3177` | `fw~pd_001` negotiated-voltage decode · `_002` negotiated-current decode · `_003` contract-state decode · `_004` bus-voltage decode | `sys~pd_001~1` |
| `dev_CYPD3177` | `fw~pd_005` init/validate + channel addressing · `_006` status-fetch pass populates state · `_007` negotiated-contract accessors · `_008` device-alive probe / not-ready handling | `sys~pd_001~1` |

(`HW_I2C` covers the HAL-for-simulation anchor `sys~arch_005~1` exactly as
`HW_SPI` does. `IO_i2c`'s spec taxonomy — topic + parent — is the one open
question, resolved when we author its spec. `lib`/`dev` CYPD3177 cover the new
`sys~pd_001~1`.)

## 6. Execution sequence — `/pcs_spec` → implement → test, per step

Dependency-ordered. Each step is a full spec→impl→test cycle, native SIL build +
`ctest` green before moving on.

0. **`sys~pd_001`** anchor spec (`/pcs_spec`). Quick; unblocks the `fw~pd` tree.
1. **`HW_I2C`** (bus) — specs → scaffold module (both targets + native/sim
   model, no HAL) → Unity tests. Wire `HW_I2C_init` into `main.c` `prvHwInit()`.
2. **`IO_i2c`** (generic device) — specs → generic per-device register
   read/write over `HW_I2C` → Unity tests. Depends on step 1.
3. **`lib_CYPD3177`** — specs → register header + pure decoders → golden-vector
   Unity tests. *Pure/independent — can run in parallel with steps 1–2.*
4. **`dev_CYPD3177`** — specs → logic using `IO_i2c` + `lib_CYPD3177` → Unity
   tests against a mock/sim `IO_i2c`. Wire init + a periodic fetch. **Constraint:**
   the fetch uses blocking I2C transfers (HW_I2C is software/polled — DMA
   deferred), so it must run on a task **below** the hard-real-time tasks (e.g.
   the FOC loop). The HAL blocking calls busy-wait, but FreeRTOS interrupt-driven
   preemption keeps higher-priority real-time contracts safe; the ~few-ms poll at
   sub-Hz rates is negligible preemptible background load.
5. **Integration** — end-to-end SIL fetch; `oft trace` clean across the new
   specs; `format.sh` scoped to branch. Merge the branch back to `main`.

## 7. Future additions (out of scope for this milestone)

This milestone delivers read-only diagnostic visibility. The generic `HW_I2C` /
`IO_i2c` stack it builds is the foundation these later features extend:

- **Feature B — HPI_INT interrupt servicing.** React to the active-low INT on
  PC13/EXTI13: read INTERRUPT (`0x0006`), dispatch to DEV_RESPONSE (`0x007E`) /
  PD_RESPONSE (`0x1400`), write-1-to-clear. Gives event-driven contract/fault
  awareness instead of poll-latency. Pulls in EXTI + an ISR→task handoff, an
  `IO_i2c` register-write path, and an EVENT_MASK (`0x1024`) config write to
  select which events fire.
- **Feature C — PD configuration / commands.** Write EVENT_MASK, SELECT_SINK_PDO
  (`0x1005`, override the strapped PDO advertisement), RESET (`0x0008`). This
  *controls* the power contract feeding the board — a full RESET renegotiates
  and can drop the board's own rail, so it is bench-only (aux supply
  recommended).
- **Other:** BUS_CURRENT (`0x1058`) sensing (needs external shunt; support
  unconfirmed on this die); GPIO_1 control over HPI.

## 8. Verification & risks

Two coverage surfaces:

- **SIL (automated, CI-enforced OFT `test`)** — HW_I2C sim loopback +
  `lib_CYPD3177` golden vectors + `dev_CYPD3177` against a mock I2C. All decode
  math is verified on known register words without hardware.
- **Bench (hardware-grounded OFT `test`)** — the bench validations SIL cannot
  cover (real I2C transaction, the *true* negotiated contract, the §2.3
  verify-on-hardware flags) are planned and logged in
  [`../hw-tests/cypd3177-i2c.md`](../hw-tests/cypd3177-i2c.md). Each case is
  executed on the bench before merge and, once run, promoted to an OFT
  `test~…` item that traces back to its spec. This is a trial convention (see
  that doc's header); if it proves out, formalize it in `docs/spec-system.md`
  (resolving its open-question #2) and wire `docs/hw-tests/` into the CI trace
  path.

**Bench note:** the board only powers up with USB-PD plugged (ST-Link alone
won't power the G431 rail) — i.e. the CYPD3177 is already negotiating before
firmware runs; the driver reads an already-live sink controller.

## 9. Status & handoff (as of cycle 4)

Branch `cypd3177-i2c`, pushed to origin. Progress against §6:

- ✅ **Step 0** `sys~pd_001` anchor (`c5dac33`).
- ✅ **Step 1** `HW_I2C` (`9d82ba5`) — `fw~hal_i2c_001..004`; HAL interrupt-mode +
  FreeRTOS per-bus semaphore *sleeping*-blocking transfers; 16 Unity tests.
- ✅ **Step 2** `IO_i2c` (`08f0b1e`) — `fw~io_i2c_001..003`; generic per-device
  layer; 14 tests; **ungated for SIM** (builds + inits on both targets).
- ✅ **Step 3** `lib_CYPD3177` (`37345f8`/`f12a1f4`) — `fw~pd_001..004`; pure
  decode; 10 golden-vector tests.
- ✅ **Step 4** `dev_CYPD3177` — `fw~pd_005..008`; runtime layer over `IO_i2c` +
  `lib_CYPD3177`; per-channel status fetch/decode/cache + accessors; 15 Unity
  tests against a bespoke mock `IO_i2c`. Wired into `main.c`: `dev_CYPD3177_init`
  + a lowest-priority `task_pd` polling `dev_CYPD3177_fetch` at 250 ms
  (sub-real-time, below all periodic tasks). Embedded-only consumer (SIM `main()`
  has no PD consumer yet); the native Unity suite carries the decode/state
  coverage regardless. ARM ELF links (49.6% flash / 57.3% RAM).
- ⬜ **Step 5** integration + bench validation.

**Decisions made during implementation (supersede the plan above where they differ):**

- `IO_i2c` specs live under a **new `io` topic** (`specs/firmware/io/i2c.md`,
  `fw~io_i2c_*`), not `hal` — resolves the §5 "ID space TBD". Raw read/write was
  **dropped** (register access only; add when a device needs it).
- `lib_CYPD3177` and `dev_CYPD3177` **share one spec file**
  `specs/firmware/pd/cypd3177.md`: lib specs under `## Register decode
  (lib_CYPD3177)`; `dev_CYPD3177` specs (`fw~pd_005..008`) go under a new
  `## Runtime status (dev_CYPD3177)` section.
- **C-symbol prefix is the module name** — `CYPD3177_*` functions (CMake target
  is `lib_CYPD3177`). For dev, follow the `dev_switch` precedent for the target
  (`dev_CYPD3177`) and symbol style.

**Gotchas the next session must know:**

- **The native full-exe build is broken pre-existingly** —
  `sw/lib/c/FreeRTOS/portable/Native-Fiber/port.c` includes `<windows.h>`, which
  fails on macOS. Unit tests are unaffected: build with
  `ninja -C build/native-fw -k 0` (past that one failure), then
  `ctest --test-dir build/native-fw -R <name>`. The full SIM executable cannot
  link on this machine. (Not our bug; flagged to the user.)
- **OFT cross-ref gotcha** — an inline `` `fw~...~1` `` must not wrap to the
  start of a line in a spec `.md` (OFT reads it as a duplicate definition).
  Always run `bash tools/oft/oft.sh trace specs/` after editing a
  cross-referencing spec; `validate-specs.py` does not catch it.
- `tools/spec_convention.py`'s ID regex was fixed to accept digit-containing
  sub-topics (`hal_i2c`, `io_i2c`).

**Decisions made during cycle 4 (supersede the plan where they differ):**

- **Specs authored via `/pcs_spec`** (interview → 2-agent research fan-out →
  fresh-context style audit → user sign-off). Four specs, edited in place under a
  new `## Runtime status (dev_CYPD3177)` section: `fw~pd_005` init + validation
  table, `fw~pd_006` status fetch (reads five registers, LE-assembles, decodes,
  caches on full success), `fw~pd_007` cached-status accessors, `fw~pd_008` device
  presence. `fw~pd_009` stays reserved for the deferred Type-C spec.
- **Accessors landed as** a single `dev_CYPD3177_isPresent`,
  `_isContractActive`, `_negotiatedVoltage_mV`, `_negotiatedCurrent_mA`,
  `_busVoltage_mV`, plus `dev_CYPD3177_fetch(channel)` (one blocking fetch pass).
  Registers read per fetch: DEVICE_MODE (alive probe), PD_STATUS, BUS_VOLTAGE,
  CURRENT_PDO, CURRENT_RDO (TYPE_C_STATUS / SILICON_ID deferred — not needed by
  the accessor set).
- **Present = one combined state**, not two: a channel is present when the most
  recent fetch read every register successfully *and* DEVICE_MODE read back
  nonzero.
- **Retain-last-good on partial read:** the cache updates only on a fully
  successful fetch; any failed read leaves the cached status unchanged and reports
  the channel not present (so a consumer reads the last valid contract plus a
  not-present flag, never a half-updated snapshot).
- **Type-C deferred:** `sys~pd_001` lists Type-C connection status, which these
  specs do not cover; the anchor stays partially covered (OFT-clean) until a later
  cycle adds `fw~pd_009`.
- **Test isolation:** the dev Unity suite mocks `IO_i2c` directly (bespoke
  `mock_IO_i2c` keyed by `(device, register)`) rather than going through the real
  `IO_i2c` to a mock `HW_I2C` — dev logic is unit-tested with no bus mechanics.
  Uses test-local seams (`IO_i2c.h`, `IO_i2c_channels.h`,
  `dev_CYPD3177_channels.h`) with two channels/devices to prove independence.

**Cycle 5 — integration + bench validation (what it needs):**

- **SIL end-to-end:** ungate a `dev_CYPD3177` consumer on the SIM target (its
  `IO_i2c` already inits on SIM) and drive a scenario that injects canned register
  bytes through the sim `HW_I2C` and asserts the decoded accessors — an end-to-end
  fetch through the real `IO_i2c`→`HW_I2C` path (the cycle-4 unit tests stop at the
  `IO_i2c` boundary).
- **Bench:** execute the cases in [`../hw-tests/cypd3177-i2c.md`](../hw-tests/cypd3177-i2c.md)
  on real silicon (real I2C transaction, true negotiated contract, the §2.3
  verify-on-hardware flags — DEVICE_MODE `0x95`, SILICON_ID, EVENT_STATUS offset,
  BUS_CURRENT presence). Promote each executed case to an OFT `test~…` item so
  `sys~pd_001`'s `test` need closes.
- **Merge:** `oft trace specs/ sw/` clean across the new specs (currently only the
  bench `test` on `sys~pd_001` is open); merge the branch back to `main`.
