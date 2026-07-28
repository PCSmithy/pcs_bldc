#!/usr/bin/env python3
"""Validate voyant SIL MF4 traces: channels present, units correct, values sane.

A round-trip check on the .mf4 files produced by the sanity suite (run it with
`PCS_SIL_TRACE_DIR=build/traces`). Opens each trace with asammdf and asserts named
channels exist with the right unit and sample counts, a known value lands where the
model says it should, and an enum channel renders text. Doubles as the stage-5
model-validation instrument.

Usage:
    python tools/validate_mf4.py [--dir build/traces]

Exits nonzero if any assertion fails.
"""

import argparse
import os
import sys

from asammdf import MDF

FAILS = []


def check(desc, ok, detail=""):
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {desc}" + (f"  ({detail})" if detail else ""))
    if not ok:
        FAILS.append(desc)


def channel(mdf, name):
    """Fetch a channel by full name, or None if absent."""
    try:
        return mdf.get(name)
    except Exception:
        return None


def check_unit(mdf, name, expected_unit, min_samples=1):
    sig = channel(mdf, name)
    if sig is None:
        check(f"{name} present", False)
        return None
    n = len(sig.samples)
    check(f"{name} present with {n} sample(s)", n >= min_samples, f"n={n}")
    check(
        f"{name} unit == {expected_unit!r}",
        sig.unit == expected_unit,
        f"got {sig.unit!r}",
    )
    return sig


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="build/traces", help="directory holding the .mf4 traces")
    args = ap.parse_args()

    e2e_path = os.path.join(args.dir, "check04_end_to_end.mf4")
    adc_path = os.path.join(args.dir, "check08_adc_ports.mf4")

    print(f"== {e2e_path} ==")
    m = MDF(e2e_path)

    angle = check_unit(m, "vsig:as5048_motor:angle", "rad")
    if angle is not None:
        last = float(angle.samples[-1])
        check(
            "angle ends at ~pi/2 (90 deg commanded -> 1.5708 rad)",
            abs(last - 1.5707963) < 1e-3,
            f"last={last:.6f}",
        )

    raw = check_unit(m, "vsig:as5048_motor:raw_encoder_ticks", "counts")
    if raw is not None:
        last = int(raw.samples[-1])
        check("raw_encoder_ticks ends at 4096 (16384/4)", last == 4096, f"last={last}")

    # PWM_U_duty: registered unitless -> asammdf must show an EMPTY unit, not a default.
    duty = check_unit(m, "vsig:pcs_bldc:PWM_U_duty", "")

    # A firmware cvar is present with samples (cvars carry no unit).
    cvar = channel(m, "cvar:pcs_bldc:task1msRuns")
    check(
        "cvar:pcs_bldc:task1msRuns present with samples",
        cvar is not None and len(cvar.samples) > 0,
        f"n={0 if cvar is None else len(cvar.samples)}",
    )

    # An enum channel renders as TEXT via its value-to-text conversion (a numeric
    # channel would come back numeric). The enumerator *names* in this firmware build
    # are DWARF placeholders (`<N>`); the pipeline carries whatever the historian
    # stored, so this asserts the mechanism (text out, not a bare number).
    enum_name = "cvar:pcs_bldc:HW_ADC_channelConfig[0].triggerMode"
    en = channel(m, enum_name)
    if en is None:
        check(f"{enum_name} present", False)
    else:
        samples = en.physical().samples  # applies the conversion
        first = samples[0]
        is_text = isinstance(first, (bytes, bytearray, str))
        rendered = first.decode() if isinstance(first, (bytes, bytearray)) else str(first)
        check(f"{enum_name} renders as text via value-to-text", is_text, f"first={rendered!r}")

    print(f"\n== {adc_path} ==")
    ma = MDF(adc_path)
    adc = check_unit(ma, "vsig:pcs_bldc:ADC1_IN6", "V")
    if adc is not None:
        check("ADC1_IN6 has a nonzero driven sample", any(float(s) != 0.0 for s in adc.samples))

    print()
    if FAILS:
        print(f"VALIDATION FAILED: {len(FAILS)} assertion(s)")
        return 1
    print("VALIDATION PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
