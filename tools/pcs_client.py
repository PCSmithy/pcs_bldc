#!/usr/bin/env python3
"""Console client for the pcs_bldc board protocol.

Opens the board's virtual COM port, streams decoded Status / log / reply
traffic to the terminal, and sends an identity + ping request on connect.
The interim bench view between Teleplot's retirement and the desktop app,
and the reference decode path for it.

Usage (from the project venv):
    .venv/Scripts/python tools/pcs_client.py --port COM5
    .venv/Scripts/python tools/pcs_client.py --selftest

Protobuf bindings are generated on demand from sw/proto/ into
tools/pcs_client_gen/ (gitignored).
"""

import argparse
import itertools
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GEN_DIR = REPO / "tools" / "pcs_client_gen"


def ensure_bindings():
    """Generate shared_pb2/board_pb2 from the schemas when missing or stale."""
    shared_dir = REPO / "sw" / "lib" / "c" / "shared" / "proto"
    board_dir = REPO / "sw" / "proto"
    protos = [shared_dir / "shared.proto", board_dir / "board.proto"]
    out = GEN_DIR / "shared_pb2.py"
    newest = max(p.stat().st_mtime for p in protos)
    if (not out.exists()) or (out.stat().st_mtime < newest):
        GEN_DIR.mkdir(exist_ok=True)
        subprocess.run(
            [sys.executable, "-m", "grpc_tools.protoc",
             f"-I{shared_dir}", f"-I{board_dir}", f"--python_out={GEN_DIR}",
             "shared.proto", "board.proto"],
            check=True,
        )
    sys.path.insert(0, str(GEN_DIR))


# --- framing (mirrors fw~conn_proto_002: COBS body of envelope ‖ CRC-32) ---

def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(seg: bytes):
    out = bytearray()
    i = 0
    while i < len(seg):
        code = seg[i]
        if code == 0 or i + code > len(seg):
            return None
        out += seg[i + 1:i + code]
        i += code
        if code != 0xFF and i < len(seg):
            out.append(0)
    return bytes(out)


def frame(envelope_bytes: bytes) -> bytes:
    plain = envelope_bytes + struct.pack("<I", crc32(envelope_bytes))
    return b"\x00" + cobs_encode(plain) + b"\x00"


def deframe(segment: bytes):
    plain = cobs_decode(segment)
    if plain is None or len(plain) < 4:
        return None
    payload, trailer = plain[:-4], plain[-4:]
    if crc32(payload) != struct.unpack("<I", trailer)[0]:
        return None
    return payload


# --- display ---

def show(env, pb2, board_pb2):
    kind = env.WhichOneof("payload")
    if kind == "telemetry":
        t = env.telemetry
        mode = board_pb2.Mode.Name(t.mode)
        state = board_pb2.DriveState.Name(t.state)
        print(f"[telemetry] t={t.timestamp_ms}ms {mode} {state} "
              f"vbus={t.bus_voltage_v:.2f}V ibus={t.bus_current_a:.3f}A "
              f"vel={t.velocity_measured_radps:+.2f}rad/s "
              f"set={t.velocity_setpoint_radps:+.2f}rad/s")
    elif kind == "log":
        print(f"[log] {env.log.text}", end="" if env.log.text.endswith("\n") else "\n")
    elif kind == "identity":
        print(f"[identity] id={env.request_id} build={env.identity.build_id}")
    elif kind == "response":
        verdict = "accepted" if env.response.accepted else f"REJECTED: {env.response.cause}"
        print(f"[reply] id={env.request_id} {verdict}")
    else:
        print(f"[?] id={env.request_id} payload={kind}")


def session(ser, pb2, board_pb2, next_id):
    """Greet the board, then stream until the port dies (SerialException)."""
    for payload in ("identity_request", "ping"):
        env = pb2.Envelope(request_id=next(next_id))
        getattr(env, payload).SetInParent()
        ser.write(frame(env.SerializeToString()))

    buffer = bytearray()
    while True:
        buffer += ser.read(4096)
        while b"\x00" in buffer:
            segment, _, buffer = buffer.partition(b"\x00")
            if not segment:
                continue
            payload = deframe(bytes(segment))
            if payload is None:
                print(f"[frame] discarded {len(segment)}-byte invalid segment")
                continue
            env = pb2.Envelope()
            try:
                env.ParseFromString(payload)
            except Exception:
                print(f"[frame] {len(payload)} bytes did not decode as Envelope")
                continue
            show(env, pb2, board_pb2)


def run(port: str, baud: int):
    import serial  # pyserial, from the venv

    ensure_bindings()
    import board_pb2
    import shared_pb2 as pb2

    # Survive board resets: when the port dies (or isn't there yet), poll for
    # re-enumeration and start a fresh session — new greeting, empty buffer.
    next_id = itertools.count(1)
    sessions = 0
    waiting_announced = False
    while True:
        try:
            with serial.Serial(port, baud, timeout=0.05) as ser:
                waiting_announced = False
                sessions += 1
                verb = "connected" if sessions == 1 else "reconnected"
                print(f"{verb} to {port}; streaming (Ctrl-C to stop)")
                session(ser, pb2, board_pb2, next_id)
        except serial.SerialException:
            if not waiting_announced:
                print(f"[port] {port} unavailable (board resetting?); waiting...")
                waiting_announced = True
            time.sleep(0.5)


def selftest() -> int:
    ensure_bindings()
    import shared_pb2 as pb2

    # The fw~conn_proto_002 reference vector: "123456789" -> CRC 0xCBF43926.
    assert crc32(b"123456789") == 0xCBF43926
    vec = frame(b"123456789")
    assert vec == bytes([0x00, 0x0E]) + b"123456789" + bytes([0x26, 0x39, 0xF4, 0xCB, 0x00])
    assert deframe(vec[1:-1]) == b"123456789"

    env = pb2.Envelope(request_id=7)
    env.ping.SetInParent()
    round_trip = pb2.Envelope()
    round_trip.ParseFromString(deframe(frame(env.SerializeToString())[1:-1]))
    assert round_trip.request_id == 7 and round_trip.WhichOneof("payload") == "ping"
    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port (e.g. COM5, /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by USB CDC")
    ap.add_argument("--selftest", action="store_true", help="verify framing + bindings offline")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())
    if not args.port:
        ap.error("--port is required (or use --selftest)")
    try:
        run(args.port, args.baud)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
