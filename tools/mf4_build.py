#!/usr/bin/env python3
"""Build an ASAM MDF4 (.mf4) trace from a voyant historian stream.

The voyant framework (Rust) serializes its State Table historian as a versioned
little-endian binary stream; this script turns that stream into a .mf4 the
asammdf GUI (and CANape / INCA) can open. Each traced signal becomes one asammdf
Signal in its own channel group (independent per-signal raster); enum signals get
a value-to-text conversion from the embedded value->name table.

Usage:
    <voyant-suite> | python tools/mf4_build.py --out trace.mf4   # stream on stdin
    python tools/mf4_build.py --in trace.bin --out trace.mf4     # a saved stream

Wire format (little-endian; mirrors sw/sil/voyant/src/trace.rs):
    header : b"VYTR" | version u32 (=1) | signal_count u32
    signal : id (u32 len + utf8) | unit (u32 len + utf8) | dtype u8 |
             sample_count u64 | timestamps (u64 us each) |
             values (native scalar each; bool->u8, enum->u32 ordinal) |
             enum table [enum only]: entry_count u32, then (ordinal u32, name)*
"""

import argparse
import struct
import sys

import numpy as np
from asammdf import MDF, Signal

MAGIC = b"VYTR"
VERSION = 1

DT_F32, DT_F64, DT_I32, DT_U32, DT_U64, DT_BOOL, DT_ENUM = range(7)
NP_DTYPE = {
    DT_F32: "<f4",
    DT_F64: "<f8",
    DT_I32: "<i4",
    DT_U32: "<u4",
    DT_U64: "<u8",
    DT_BOOL: "u1",
    DT_ENUM: "<u4",
}


class Reader:
    """A cursor over the byte stream with little-endian primitive readers."""

    def __init__(self, data):
        self.data = data
        self.off = 0

    def take(self, n):
        end = self.off + n
        if end > len(self.data):
            raise ValueError("truncated trace stream")
        chunk = self.data[self.off:end]
        self.off = end
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack_from("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack_from("<Q", self.take(8))[0]

    def string(self):
        return self.take(self.u32()).decode("utf-8")


def value_to_text(pairs):
    """An asammdf value-to-text conversion dict from (ordinal, name) pairs."""
    conv = {}
    for i, (ordinal, name) in enumerate(pairs):
        conv[f"val_{i}"] = int(ordinal)
        conv[f"text_{i}"] = name
    return conv


def parse(data):
    """Parse the stream into a list of asammdf Signals."""
    r = Reader(data)
    if r.take(4) != MAGIC:
        raise ValueError("bad magic (not a voyant trace stream)")
    version = r.u32()
    if version != VERSION:
        raise ValueError(f"unsupported trace version {version}")
    count = r.u32()

    signals = []
    for _ in range(count):
        name = r.string()
        unit = r.string()
        dtype = r.u8()
        n = r.u64()
        # us -> seconds (asammdf masters are seconds, float64).
        times = np.frombuffer(r.take(8 * n), dtype="<u8").astype(np.float64) / 1e6
        width = np.dtype(NP_DTYPE[dtype]).itemsize
        values = np.frombuffer(r.take(width * n), dtype=NP_DTYPE[dtype]).copy()

        conversion = None
        if dtype == DT_ENUM:
            pairs = [(r.u32(), r.string()) for _ in range(r.u32())]
            conversion = value_to_text(pairs)

        signals.append(
            Signal(
                samples=values,
                timestamps=times,
                name=name,
                unit=unit,
                conversion=conversion,
            )
        )
    if r.off != len(data):
        raise ValueError("trailing bytes after the last signal")
    return signals


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output .mf4 path")
    ap.add_argument("--in", dest="infile", help="read the stream from a file (default: stdin)")
    args = ap.parse_args()

    data = open(args.infile, "rb").read() if args.infile else sys.stdin.buffer.read()
    signals = parse(data)

    mdf = MDF(version="4.10")
    # One group per signal keeps each signal on its own timestamp raster.
    for sig in signals:
        mdf.append([sig], comment=sig.name)
    mdf.save(args.out, overwrite=True)
    print(f"mf4_build: wrote {len(signals)} signal(s) -> {args.out}")


if __name__ == "__main__":
    main()
