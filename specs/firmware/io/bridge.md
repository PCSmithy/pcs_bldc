---
status: draft
tags: [firmware, io, bridge, driver]
---

# Bridge io-layer driver

The `IO_bridge` driver is the generic io-layer three-phase motor-bridge
abstraction over `HW_TIM`. It presents normalized per-phase duty commands, a
stationary-frame voltage-vector command, a per-phase output enable, and a
whole-bridge output enable, addressed by logical bridge and phase; each phase
maps to a configured HW_TIM logical channel, and a bridge's three phases share
one HW_TIM peripheral whose master output enable gates the bridge.

See also: [[overview]] (`sys~arch_005~1`), [[tim]] (HW_TIM supplies the
complementary PWM), [[bridge-actuation]] (`sys~mc_004~1`), [[modulation]]
(`fw~mc_014~1` turns the voltage vector into duties),
[[motor-control-application]] (`fw~mc_015~1` runs the commutation step from
the per-cycle callback).

## Driver configuration and lifecycle

### Initialization and configuration validation
`fw~io_bridge_001~1`

The driver shall validate the supplied configuration and initialize every
configured bridge, returning false on any of:

| Element | Rejected when |
|---------|---------------|
| Config  | The config pointer or its channel array is NULL, or the bridge count exceeds the available bridges. |
| Phase   | A phase's HW_TIM channel is out of range. |
| Bridge  | The three phases do not resolve to one shared HW_TIM peripheral. |
| Bridges | An ADC channel carries injected phase senses of more than one bridge. |
| Phase   | A phase's complement is itself, or names a phase whose own complement is not it (the derived phase carries none). |
| Bridge  | The zero-current offset weight is outside (0, 1], or its minimum run length is zero samples (`fw~io_bridge_009~1`). |

Acceptance:
- A valid configuration initializes every bridge, enables each phase's
  output-compare unit, leaves the master output enable clear, and returns true.
- Each rejection condition returns false.

Covers:
- `sys~arch_005~1`

Needs: impl, test

## Bridge actuation

### Per-phase duty command
`fw~io_bridge_002~1`

The driver shall apply a per-phase duty command in [0, 1] to the phase's
output-compare unit as the fraction of the PWM period during which the phase's
primary output is active, returning false for a command outside [0, 1], an
out-of-range bridge or phase, or an uninitialized driver.

Acceptance:
- Duty commands of 0, 0.5, and 1 produce compare values of zero, half, and the
  full timer period on the phase's output-compare unit.
- Each rejected command returns false and leaves every compare value unchanged.

Covers:
- `sys~mc_004~1`

Needs: impl, test

### Bridge output enable
`fw~io_bridge_003~1`

The driver shall set and report the whole-bridge output-enable state through the
phases' shared master output enable (`fw~hal_tim_008~1`): while disabled, every
phase output holds its inactive state and accepted duty commands take effect on
the outputs at re-enable, and the reported state includes a disable forced by
the peripheral's break input.

Acceptance:
- Disabling holds all phase outputs inactive.
- Duty commands issued while disabled return true and take effect at re-enable.
- With no break asserted, the reported state matches the commanded state.
- The reported state reads disabled after a break-input assertion.

Covers:
- `sys~mc_004~1`

Needs: impl, test

### Per-phase output enable
`fw~io_bridge_004~1`

The driver shall enable or disable one phase's output independently, a disabled
phase holding both its gate lines at the inactive level while the other phases
keep driving.

Acceptance:
- Disabling one phase holds that phase's outputs inactive and leaves the other
  phases' outputs driving.
- Re-enabling the phase restores its output.
- A command on an out-of-range phase returns false.

Covers:
- `sys~mc_004~1`

Needs: impl, test

### Voltage-vector command
`fw~io_bridge_008~1`

The driver shall apply a stationary-frame voltage command (v_α, v_β) to a
bridge as the three per-phase duties `fw~mc_014~1` produces from it and the
bridge's bus voltage (`fw~io_bridge_005~1`), returning false, with every
phase's duty unchanged, for a non-finite command or a failed bus-voltage
readout.

Acceptance:
- Each phase's compare value is within one count of its `fw~mc_014~1` duty,
  for the command and the bus voltage read at the call, times the PWM
  period.
- A rejected command returns false and leaves every compare value
  unchanged.

Covers:
- `sys~mc_004~1`

Needs: impl, test

## Current and voltage sense

### Sense readout in engineering units
`fw~io_bridge_005~1`

The driver shall report each configured sense — the three phase currents,
the bus current, and the bus voltage — as its decoded value

value = (V_pin − V_bias) / scale

with V_bias and scale the sense's configured bias and volts per unit,
less, for a phase current, its zero-current offset (`fw~io_bridge_009~1`),
the sample each reads per:

| Sense | Sample reported |
|-------|-----------------|
| Phase current | The most recently completed injected-sequence sample of that phase — U and V as sampled at the PWM crest, W as derived (`fw~io_bridge_006~1`) — never a regular-sequence sample; zero before the first |
| Bus current, bus voltage | The most recent regular-sequence sample of its ADC input |

returning false for a sense whose scale is zero, an out-of-range bridge or
phase, or an uninitialized driver.

Acceptance:
- With the phase zero-current offsets (`fw~io_bridge_009~1`) at zero, a pin
  voltage of V_bias reads zero and a pin voltage of V_bias + scale reads one
  unit.
- A sense with zero scale returns false.
- A new injected sample changes the reported phase current at the next read;
  a new regular sample of the same input does not.
- A new regular sample of a bus sense's input changes that sense's reported
  value at the next read.

Covers:
- `sys~mc_001~1`

Needs: impl, test

### Injected phase-current cycle
`fw~io_bridge_006~1`

The driver shall form each PWM cycle's phase-current triple from the
injected U and V samples whose timestamps fall within the configured pair
window — U and V in amps per `fw~io_bridge_005~1`'s law, W = −(U + V) — with
a sample arriving outside the window starting a new pair.

Acceptance:
- Over N PWM periods, N triples are formed, each from that period's
  samples.
- Two samples farther apart than the pair window form no triple.

Covers:
- `sys~mc_001~1`

Needs: impl, test

### Per-cycle callback
`fw~io_bridge_007~1`

The driver shall invoke a callback registered per bridge, with its
registered context, once per completed phase-current triple
(`fw~io_bridge_006~1`), in the interrupt context of the completing injected
conversion.

Acceptance:
- Over N completed triples the callback runs N times with the registered
  context, each after that triple is readable.

Covers:
- `sys~mc_001~1`

Needs: impl, test

### Zero-current offset tracking
`fw~io_bridge_009~1`

The driver shall hold a zero-current offset per phase, the term that the
sense readout (`fw~io_bridge_005~1`) subtracts, set per:

| At | Offset |
|----|--------|
| Init | Zero |
| A driving phase's first injected sample after an idle run of at least the configured minimum run length | y |
| Any other sample | Unchanged |

A phase is idle while its output is disabled or the bridge's master output
enable is off. An idle run is the phase's consecutive injected samples
while idle. y is the exponential average of the phase's decoded current i
over the run: seeded to i on the run's first sample and advanced
y ← y + α (i − y) on each later one. The weight α and the minimum run
length, in samples, are configured per bridge.

Acceptance:
- With the bridge disabled and the decoded phase currents held at
  (I_u, I_v, −I_u − I_v) through the minimum run length and through the
  next enable, the phase currents reported after that enable are zero,
  within 10 mA.
- After an idle run shorter than the minimum, the phase currents reported
  after the next enable are the decoded currents less the offsets held
  before the run, within 10 mA.
- Two idle runs of the minimum length at different held levels, then an
  enable with the decoded currents still at the second level, report zero
  within 10 mA.
- With the bridge driving, one phase's output disabled through the minimum
  run length at a held decoded current, and that current still held
  once its output is re-enabled, the phase reports zero within 10 mA while
  the other two report their decoded currents less the offsets they held
  before.
- After an idle run of N samples with a decoded current of I on the first
  and zero on the rest, the phase's reported current once driving with its
  decoded current at zero is −I (1 − α)^(N−1), within 1 %.

Covers:
- `sys~mc_001~1`

Needs: impl, test
