---
status: draft
tags: [firmware, obs, rgb_led_ring, app]
---

# RGB LED ring UI

The app-layer ring UI drives the on-device RGB LED ring. It runs as a periodic
task that reads the dial and motor encoders (`IO_AS5048`) and the debounced
user button (`DEV_switch`), and renders to the ring through the `IO_SK6805`
string driver.

See also: [[overview]] (sys~arch_001~1), [[rgb_leds]] (IO_SK6805 driver),
[[encoder]] (IO_AS5048 driver).

### Periodic operation and mode selection
`fw~obs_ring_001~1`

The ring UI shall run a fixed 10 ms cycle that reads the latest dial and motor
encoder angles and the debounced user-button state, renders the active display
mode, and transmits the resulting frame to the ring. The button advances the
active mode through the cycle WALK → SOLID → SOLID2 → ENCODER → OFF → WALK on
each rising edge, and each advance flashes the whole ring white (50 ms on,
50 ms off) before the new mode renders.

Acceptance:
- Frames are transmitted to the ring at a 10 ms period, each built from the
  latest encoder angles and button state.
- Five successive button presses step the active mode WALK, SOLID, SOLID2,
  ENCODER, OFF, and a sixth returns to WALK; the mode advances once per press.
- A mode advance shows the whole ring white for 50 ms, then off for 50 ms,
  before the new mode's first frame.

Covers:
- sys~arch_001~1

Needs: impl, test

### Per-mode rendering
`fw~obs_ring_002~1`

The ring UI shall render the active mode to the ring each cycle as defined
below. A *pip* is a soft dot centred on a ring angle whose brightness falls
linearly from full at the centre to zero at ±18°, measured the short way
around the ring; where pips overlap, their colour channels add and clamp at
full intensity.

| Mode | Rendering |
|------|-----------|
| WALK | Two pips orbit the ring. The dial and motor encoders each drive one pip's signed speed through a sticky accumulator of encoder movement, clamped to ±340° of travel and mapped linearly to a signed speed up to ±2880 deg/s; zero accumulation holds the pip still. The dial pip uses the SOLID colour, the motor pip the SOLID2 colour. |
| SOLID / SOLID2 | The whole ring shows one colour from a two-encoder HSV picker: motor movement winds hue 1:1 wrapping at the 0/360° seam, and dial movement drives saturation across 180° of travel from white to full. SOLID and SOLID2 are independent pickers, starting at red and blue respectively. |
| ENCODER | One pip tracks the dial angle in the SOLID colour and one tracks the motor angle in the SOLID2 colour. |
| OFF | Every pixel is off. |

Acceptance:
- WALK: a still encoder holds its pip fixed; sustained winding moves the pip
  at a speed that rises with accumulated travel to a ±2880 deg/s bound, and
  once the accumulator is saturated a reversal turns the pip around
  immediately.
- SOLID and SOLID2: full motor travel returns the ring to its starting hue;
  dial travel spans white to full saturation.
- ENCODER: each pip peaks within one LED of its encoder angle and falls to
  zero beyond ±18°, and overlapping pips sum per channel and clamp at full.
- OFF: no pixel is lit for any encoder or button input.

Covers:
- sys~arch_001~1

Needs: impl, test

### Colour persistence across modes
`fw~obs_ring_003~1`

The SOLID and SOLID2 colours shall persist across mode changes: each picker
resumes the colour it last held, and only the active picker's colour responds
to encoder movement.

Acceptance:
- Returning to SOLID after using other modes shows the colour SOLID last held;
  the same holds for SOLID2.
- Encoder movement updates only the active picker's colour; on return, each
  picker shows the colour it last held.

Covers:
- sys~arch_001~1

Needs: impl, test
