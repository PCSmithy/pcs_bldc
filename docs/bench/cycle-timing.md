# Bench record: PWM-cycle durations

Written by `tools/bench/cycle_timing.py`; each run replaces it.

- Date: 2026-11-14
- Firmware identity: `b9974225a05a`
- Window: 60.0 s, 71333 core samples
- Result: **fail**

| Check | Measured | Result |
|---|---|---|
| drive in the Driving state at every look, no fault latched | 0 looks not driving, fault 0 | pass |
| speed at the 18.0 rad/s maximum target (within 5 %) | mean |velocity| 18.05 rad/s | pass |
| 32 trace watches active | 32 active | pass |
| commutation step maximum within 20 us | 21 us | **fail** |
| callback maximum within 40 us | 42 us | **fail** |
| interrupt maximum restarted at the clear and bounds every latest duration seen | max 50 us, largest latest seen 48 us | pass |
| interrupt total agrees with the time the core spends in the handler (within 1.27 us per run) | total 32.01 us per run, core 33.16 us per run over 71333 samples | pass |
