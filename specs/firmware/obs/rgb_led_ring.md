---
status: draft
tags: [firmware, obs, rgb_led_ring, app]
---

# RGB LED ring UI

The app-layer ring UI drives the on-device RGB LED ring. It runs as a periodic
task that reads the dial and motor encoders (`IO_AS5048`) and renders to the
ring through the `IO_SK6805` string driver. Its display mode is advanced on
demand through `app_rgbLedRing_cycleMode`; the user button that drives that
call is owned by the user-controls gesture layer, not the ring.

See also: [[overview]] (sys~arch_001~1), [[rgb_leds]] (IO_SK6805 driver),
[[encoder]] (IO_AS5048 driver), [[motor-control-application]] (fw~mc_007~1, the
button gesture that invokes the cycle).

### Periodic operation and mode selection
`fw~obs_ring_001~1`

The ring UI shall run a fixed 10 ms cycle that reads the latest dial and motor
encoder angles, renders the current display, and transmits the resulting frame
to the ring. While the motor runs, a `cycleMode` request toggles the active
view POSITION ↔ SPEEDO, and each advance flashes the whole ring white (50 ms
on, 50 ms off) before the new view renders.

Acceptance:
- Frames are transmitted to the ring at a 10 ms period, each built from the
  latest encoder angles.
- With the motor running, successive cycleMode requests toggle the active view
  POSITION, SPEEDO, POSITION; the view advances once per request.
- A view advance shows the whole ring white for 50 ms, then off for 50 ms,
  before the new view's first frame.

Covers:
- sys~arch_001~1

Needs: impl, test

### State display and per-view rendering
`fw~obs_ring_002~1`

The ring shall render each cycle by motor state: a faulted motor fills the ring
red, a disabled (off) motor blanks it, and a running motor renders the active
view. A *pip* is a soft dot centred on a ring angle whose brightness falls
linearly from full at the centre to zero at ±18°, measured the short way
around the ring; overlapping pips add per channel and clamp at full. The LED
order runs opposite the dial/motor sense, so a shaft angle maps to its
complement on the ring.

| View | Rendering |
|------|-----------|
| POSITION | A green pip tracks the motor shaft angle and a blue pip the dial angle, each at the complemented ring angle. |
| SPEEDO | Two needles on a speedometer arc (zero at top, ±full sweeping ±150°): a magenta pip at the commanded speed setpoint and a green pip at the measured actual speed, each normalised to the configured full-scale speed. |

Acceptance:
- A faulted motor lights every pixel red; a disabled motor lights none,
  regardless of view or encoder input.
- POSITION: the motor and dial pips each peak within one LED of their
  complemented angle and fall to zero beyond ±18°.
- SPEEDO: the setpoint needle sits at the arc position for its speed (full
  scale → +150°); the actual needle sits at the position for the measured
  speed.

Covers:
- sys~arch_001~1

Needs: impl, test
