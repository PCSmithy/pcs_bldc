---
status: draft
tags: [firmware, obs, cpu]
---

# CPU time accounting

See also: [[../conn/trace]] (the trace carries the counts),
[[../hal/adc]] (`fw~hal_adc_011~1`, the PWM-cycle interrupt's own total).

### Per-task execution time
`fw~obs_cpu_001~1`

The firmware shall hold, as readable statics, one cumulative count of
microseconds of execution per slot, charging each run from a task's
switch-in to the next switch-in — interrupt time included — measured on
the bridge's free-running microsecond time base, modulo 2^32, with the
slots

| Slot | Task |
|------|------|
| 0 | any task without a slot (the kernel's timer daemon) |
| 1 | the idle task |
| 2 | the 1 ms task |
| 3 | the 10 ms task |
| 4 | the 200 ms task |
| 5 | the USB device task |
| 6 | the protocol server task |

Acceptance:
- Over a sequence of switch-ins at known times, each slot's count equals
  the sum of that task's runs and the counts' sum equals the elapsed
  time; a time-base wrap inside a run charges the true span.
- Over a 1 s window of the running firmware, the slots' advances on the
  same time base sum to the window's duration within one task run.
- With the motor not driven, slot 1 advances more than any other slot.

Covers:
- sys~obs_010~1

Needs: impl, test
