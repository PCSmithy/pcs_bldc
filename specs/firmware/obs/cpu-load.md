---
status: draft
tags: [firmware, obs, cpu]
---

# CPU time accounting

Cumulative execution time per task, kept by `app_cpuLoad` from the
kernel's switch-in hook on the free-running microsecond time base, readable
as statics for the trace. The PWM-cycle interrupt's total is the ADC
driver's (`fw~hal_adc_011~1`); interrupt time is charged to the task it
interrupts, which the host corrects for.

See also: [[../hal/adc]] (the interrupt total), [[../conn/serial]]
(the trace that carries the signals).

### Per-task execution time
`fw~obs_cpu_001~1`

The firmware shall hold, as readable statics, one cumulative count of
microseconds of execution per task slot — the idle task and each created
task assigned a slot, tasks without one sharing slot zero — charging each
run from a task's switch-in to the next switch-in, measured on the
free-running microsecond time base, modulo 2^32.

Acceptance:
- Over a sequence of switch-ins at known times, each slot's count equals
  the sum of that task's runs, and the counts' sum equals the elapsed
  time; a time-base wrap inside a run charges the true span.
- Over any 1 s window of the running firmware, the slots' advances sum to
  the window's duration within 1 %.

Covers:
- sys~obs_010~1

Needs: impl, test
