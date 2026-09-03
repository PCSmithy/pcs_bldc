#!/usr/bin/env python3
"""Log board telemetry from a serial port to CSV, unbounded.

Reads the firmware's Teleplot-style stream (`name:t_ms:value<U+00A7>unit`,
entries `;`-separated, lines `\\n`-terminated) and appends one CSV row per
entry in the same format the trace_analysis notebooks load:

    signalName,t,tWall,value,unit

with `signalName` = `serial:<PORT>/<name>`, `t` the board's ms timestamp, and
`tWall` the host's epoch ms. Rows are flushed each second, so a crash or
Ctrl+C loses at most a second. Malformed / non-numeric entries are skipped and
counted. A board reset mid-capture (USB CDC re-enumeration) is survived: the
script re-attaches when the port returns and keeps appending to the same CSV.

Usage:
    python tools/serial_capture.py COM8
    python tools/serial_capture.py COM8 --out mytest.csv --baud 115200

Requires pyserial (`pip install pyserial`, in requirements.txt).
"""

import argparse
import csv
import sys
import time
from collections import Counter
from datetime import datetime

import serial

UNIT_SEP = "§"  # the firmware's TP_UNIT separator between value and unit


def parse_entry(entry: str):
    """One `name:t:value[<sep>unit]` entry -> (name, t, value, unit) or None."""
    parts = entry.split(":", 2)
    if len(parts) != 3:
        return None
    name, t_str, rest = parts
    value_str, _, unit = rest.partition(UNIT_SEP)
    try:
        t = int(t_str)
        float(value_str)  # numeric guard; the CSV keeps the exact text
    except ValueError:
        return None
    return name.strip(), t, value_str.strip(), unit.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="serial port (e.g. COM8, /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200,
                    help="baud rate (USB CDC ignores it; default 115200)")
    ap.add_argument("--out", default=None,
                    help="output CSV (default capture_<timestamp>.csv)")
    ap.add_argument("--quiet", action="store_true", help="suppress the 5 s status line")
    args = ap.parse_args()

    out_path = args.out or f"capture_{datetime.now():%Y%m%d_%H%M%S}.csv"
    prefix = f"serial:{args.port}/"

    rows = 0
    skipped = 0
    per_signal = Counter()
    started = time.monotonic()
    last_status = started

    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["signalName", "t", "tWall", "value", "unit"])
        sp = serial.Serial(args.port, args.baud, timeout=1)   # fail fast on a bad port
        print(f"logging {args.port} -> {out_path}  (Ctrl+C to stop)", file=sys.stderr)
        try:
            buf = b""
            while True:
                try:
                    chunk = sp.read(4096)
                except serial.SerialException:
                    # A board reset re-enumerates USB CDC and kills the handle:
                    # drop the partial line, keep the CSV, re-attach when the
                    # port comes back.
                    print(f"  {args.port} dropped (board reset?) — reattaching",
                          file=sys.stderr)
                    try:
                        sp.close()
                    except serial.SerialException:
                        pass
                    buf = b""
                    while True:
                        time.sleep(0.5)
                        try:
                            sp = serial.Serial(args.port, args.baud, timeout=1)
                            break
                        except serial.SerialException:
                            continue
                    print(f"  {args.port} reattached", file=sys.stderr)
                    continue
                if chunk:
                    buf += chunk
                    *lines, buf = buf.split(b"\n")
                    t_wall = int(time.time() * 1000)
                    for line in lines:
                        for entry in line.decode("utf-8", errors="replace").split(";"):
                            entry = entry.strip()
                            if not entry:
                                continue
                            parsed = parse_entry(entry)
                            if parsed is None:
                                skipped += 1
                                continue
                            name, t, value, unit = parsed
                            w.writerow([prefix + name, t, t_wall, value, unit])
                            rows += 1
                            per_signal[name] += 1
                now = time.monotonic()
                if now - last_status >= 1.0:
                    f.flush()
                    if (not args.quiet) and (now - last_status >= 5.0 or last_status == started):
                        rate = rows / (now - started) if now > started else 0.0
                        print(f"  {rows} rows ({rate:.0f}/s), {len(per_signal)} signals, "
                              f"{skipped} skipped", file=sys.stderr)
                        last_status = now
        except KeyboardInterrupt:
            pass
        finally:
            try:
                sp.close()
            except serial.SerialException:
                pass

    dur = time.monotonic() - started
    print(f"\nstopped after {dur:.1f} s: {rows} rows, {skipped} skipped -> {out_path}",
          file=sys.stderr)
    for name, n in sorted(per_signal.items()):
        print(f"  {name:16s} {n:8d} samples", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
