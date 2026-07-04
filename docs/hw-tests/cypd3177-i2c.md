# HW test plan & execution record — CYPD3177 / I2C milestone

Bench-validation plan and execution log for the `cypd3177-i2c` milestone
(see [`../milestones/cypd3177-i2c.md`](../milestones/cypd3177-i2c.md)). It
records the hardware tests that give confidence in specs SIL alone cannot cover
— the real I2C transaction against real silicon and the *true* negotiated PD
contract — and traces executed runs back to the specs via OpenFastTrace.

## How this works (trial convention)

SIL/unit tests remain the automated OFT `test` coverage that CI enforces. This
doc adds a *second*, hardware-grounded coverage source for the specs that
warrant it. Not every spec needs a HW test.

- **Planned case** — a heading, the spec it will cover written as plain prose
  (`Covers on execution: <spec-id>`), a procedure, and the expected result. It
  carries **no** OFT item ID, so it provides **no** coverage. A written-but-unrun
  test cannot fake traceability.
- **Executed case** — on a real bench run, the case is promoted: it gains an OFT
  item ID line `` `test~<topic>_hw_<NNN>~1` ``, a dated **Executions** log entry
  (result, board rev, conditions), and a real `Covers:` block. OFT then counts
  it as `test` coverage of the linked spec.

These `test~…` items live here, outside `specs/`, so `tools/validate-specs.py`
(which governs `sys`/`fw`/`app` requirement IDs) does not touch them — they are
test artifacts, like the `// [test->…]` tags in `.c` unit tests.

**Tracing with HW records included:**

```sh
tools/oft/oft.sh trace specs/ sw/lib sw/fw docs/hw-tests/
```

The path `docs/hw-tests/` must be added to the trace invocation for these
records to count. Until a case is executed and promoted, this doc contributes
nothing to the trace (planned cases are inert prose).

## Planned cases — `sys~pd_001~1` (USB-PD sink status monitoring)

These validate the spec's acceptance against physical ground truth. They become
runnable once `dev_CYPD3177` lands (milestone step 4) and are executed on the
bench before the milestone merges.

### HWT-PD-001 — Negotiated contract matches a known source
Covers on execution: `sys~pd_001~1`
Procedure: power the board from a USB-PD source of known capability (e.g. a
20 V / 3 A adapter or programmable USB-PD tester); read the exposed negotiated
voltage and current.
Expected: exposed negotiated voltage and current equal the contract actually
negotiated (consistent with the board's strapped VBUS_MIN/MAX and ISNK request).
Executions: none yet.

### HWT-PD-002 — Live VBUS matches meter
Covers on execution: `sys~pd_001~1`
Procedure: measure the VBUS rail with a calibrated multimeter (or scope) at the
same instant the exposed VBUS voltage is read.
Expected: exposed VBUS within ±0.1 V of the meter reading.
Executions: none yet.

### HWT-PD-003 — Type-C connection status matches physical attach
Covers on execution: `sys~pd_001~1`
Procedure: read the exposed Type-C connection status with the USB-C cable
attached, and again after detach/reattach.
Expected: exposed Type-C connection status matches the physical attach state.
Executions: none yet.

### HWT-PD-004 — Reachability reflects controller responsiveness
Covers on execution: `sys~pd_001~1`
Procedure: in normal operation, read the exposed reachability status; then induce
a non-responding condition on the HPI bus (bus-fault injection / scope-observed
NAK). Note: the CYPD3177 supplies the board rail, so a full power-loss case is
not exercised here.
Expected: reachability reports available when the controller responds and
unavailable when it does not.
Executions: none yet.

### HWT-PD-005 — Device-alive probe on real silicon
Covers on execution: `sys~pd_001~1`
Procedure: on real hardware, exercise the device-alive probe and capture the raw
DEVICE_MODE (`0x0000`) value; confirm the I2C transaction completes with the
peripheral's clock stretching enabled.
Expected: the probe reports the controller present; the raw DEVICE_MODE reads as
the real-silicon value (`0x95`, per the Infineon-confirmed erratum), closing the
milestone's verify-on-hardware flag.
Executions: none yet.

## Executed cases

(none yet — promoted here from the plan above as bench runs are performed.)
