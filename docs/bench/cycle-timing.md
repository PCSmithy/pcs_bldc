# Bench record: PWM-cycle durations

Written by `tools/bench/cycle_timing.py`; each run replaces it.

- Date: 2026-11-17
- Firmware identity: `367632eb7bf4`
- Window: 60.0 s, 69313 core samples
- Result: **pass**

| Check | Measured | Result |
|---|---|---|
| drive in the Driving state at every look, no fault latched | 0 looks not driving, fault 0 | pass |
| speed at the 18.0 rad/s maximum target (within 5 %) | mean |velocity| 18.13 rad/s | pass |
| 32 trace watches active | 32 active | pass |
| commutation step maximum within 20 us | 15 us | pass |
| callback maximum within 40 us | 32 us | pass |
| no PWM cycle missed | 0 missed | pass |
| cycles missed only when the interrupt maximum nears the 50 us period | 0 missed, interrupt max 38 us | pass |
| interrupt maximum restarted at the clear and bounds every latest duration seen | max 38 us, largest latest seen 37 us | pass |
| interrupt total agrees with the time the core spends in the handler (within 1.28 us per run) | total 23.33 us per run, core 23.81 us per run over 69313 samples | pass |
