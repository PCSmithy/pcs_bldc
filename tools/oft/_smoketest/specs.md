---
status: example
tags: [smoketest]
---

# Smoketest specs

These are not real project specs — they exercise the OFT install and
serve as a working example of the spec format. Run from the repo root:

    ./tools/oft/oft.sh trace tools/oft/_smoketest/

Expected output: `ok - 5 total`, exit 0.

### Motor spins
`sys~smoketest_motor_spins~1`

The system shall spin the motor when commanded.

Needs: fw, test

### PWM init
`fw~smoketest_pwm_init~1`

The PWM peripheral shall be initialized at boot.

Covers:
- sys~smoketest_motor_spins~1

Needs: impl, test
