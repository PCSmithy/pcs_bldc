#!/usr/bin/env python3
"""Console client for the pcs_bldc board protocol.

Opens the board's virtual COM port, streams decoded Status / log / reply
traffic to the terminal, and sends an identity + ping request on connect.
The interim bench view between Teleplot's retirement and the desktop app,
and the reference decode path for it.

Usage (from the project venv):
    .venv/Scripts/python tools/pcs_client.py --port COM5
    .venv/Scripts/python tools/pcs_client.py --port COM5 --watch 0x20000110:4:20
    .venv/Scripts/python tools/pcs_client.py --port COM5 --link-test 256:20000
    .venv/Scripts/python tools/pcs_client.py --selftest

Protobuf bindings are generated on demand from sw/lib/c/shared/proto/ and
sw/proto/ into tools/pcs_client_gen/ (gitignored).
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

# Silence after the requested count that ends a link test: long enough that a
# stalled USB transfer isn't mistaken for the end of the stream.
LINK_TEST_QUIET_S = 2.0


def grow_rx_buffer(ser):
    """Windows usbser keeps a 4 KB driver buffer by default; at hundreds of kB/s
    a few ms of host latency overruns it and corrupts frames. Harmless elsewhere."""
    if hasattr(ser, "set_buffer_size"):
        ser.set_buffer_size(rx_size=1 << 20)


def ensure_bindings():
    """Generate shared_pb2/board_pb2 from the schemas when missing or stale."""
    shared_dir = REPO / "sw" / "lib" / "c" / "shared" / "proto"
    board_dir = REPO / "sw" / "proto"
    protos = [shared_dir / "shared.proto", shared_dir / "trace.proto", board_dir / "board.proto"]
    outs = [GEN_DIR / f"{name}_pb2.py" for name in ("shared", "trace", "board")]
    newest = max(p.stat().st_mtime for p in protos)
    if any((not o.exists()) or (o.stat().st_mtime < newest) for o in outs):
        GEN_DIR.mkdir(exist_ok=True)
        subprocess.run(
            [sys.executable, "-m", "grpc_tools.protoc",
             f"-I{shared_dir}", f"-I{board_dir}", f"--python_out={GEN_DIR}",
             "shared.proto", "trace.proto", "board.proto"],
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


def split_frames(buffer: bytearray):
    """Split a read at every delimiter: whole segments plus the trailing
    partial. One split per read beats rescanning the buffer per frame."""
    segments = buffer.split(b"\x00")
    tail = bytearray(segments.pop())
    return [bytes(seg) for seg in segments if seg], tail


# --- display ---

def show(env, pb2, board_pb2, log_buffer, trace=None):
    kind = env.WhichOneof("payload")
    if kind == "samples":
        m = env.samples
        # Advance the expectation before any early return: a message we can't
        # decode still consumed its records, and skipping it fakes a gap on
        # the next one. Cycle index is a uint32 and wraps.
        expected = None
        if trace is not None:
            expected = trace["next_cycle"].get(m.period_cycles)
            trace["next_cycle"][m.period_cycles] = (
                (m.first_cycle + (m.count * m.period_cycles)) & 0xFFFFFFFF)
        if trace is None or not trace.get("watches"):
            print(f"[samples] c={m.first_cycle} x{m.count} @{m.period_cycles} "
                  f"{len(m.data)}B (no local watch list)")
            return
        # One message carries one group: the watches of that period, in list
        # order, repeated `count` times (fw~conn_trace_005).
        group = [w for w in trace["watches"] if w[2] == m.period_cycles]
        record = sum(w[1] for w in group)
        if not group or m.count == 0 or record * m.count != len(m.data):
            print(f"[samples] c={m.first_cycle} x{m.count} @{m.period_cycles} "
                  f"{len(m.data)}B (does not match the local watch list)")
            return

        # Per-group gap detection: the group's cycle indices step by its period.
        if (expected is not None) and (m.first_cycle != expected):
            dropped = ((m.first_cycle - expected) & 0xFFFFFFFF) // m.period_cycles
            print(f"[samples] GAP @{m.period_cycles}: expected c={expected}, "
                  f"got c={m.first_cycle} ({dropped} record(s) dropped)")

        # Print when a multiple of print_every falls inside this batch.
        seen = trace.get("count", 0)
        trace["count"] = seen + m.count
        if (-seen) % trace.get("print_every", 100) >= m.count:
            return
        # Print the batch's newest record; the rest rode the same message.
        k = m.count - 1
        offset = k * record
        parts = []
        for addr, size, _period, fmt in group:
            chunk = m.data[offset:offset + size]
            offset += size
            if fmt == "f":
                value = struct.unpack("<f" if size == 4 else "<d", chunk)[0]
                parts.append(f"0x{addr:08X}={value:+.4f}")
            else:
                parts.append(f"0x{addr:08X}=0x{int.from_bytes(chunk, 'little'):0{size * 2}X}")
        print(f"[samples] c={m.first_cycle + k * m.period_cycles} " + " ".join(parts))
        return
    if kind == "trace_status":
        s = env.trace_status
        print(f"[trace] id={env.request_id} ram {s.ram_usage_bytes_per_ms}/{s.ram_budget_bytes_per_ms}B/ms "
              f"link {s.link_rate_bytes_per_s}/{s.link_budget_bytes_per_s}B/s")
        return
    if kind == "read_reply":
        print(f"[read] id={env.request_id} {env.read_reply.data.hex()}")
        return
    if kind == "log":
        # LogText chunks split without regard to line boundaries; buffer and
        # emit whole lines so every heartbeat gets its own [log] prefix.
        log_buffer.append(env.log.text)
        text = "".join(log_buffer)
        log_buffer.clear()
        while "\n" in text:
            line, _, text = text.partition("\n")
            print(f"[log] {line}")
        if text:
            log_buffer.append(text)
        return
    if kind == "telemetry":
        t = env.telemetry
        mode = board_pb2.Mode.Name(t.mode)
        state = board_pb2.DriveState.Name(t.state)
        print(f"[telemetry] t={t.timestamp_ms}ms {mode} {state} "
              f"vbus={t.bus_voltage_v:.2f}V ibus={t.bus_current_a:.3f}A "
              f"vel={t.velocity_measured_radps:+.2f}rad/s "
              f"set={t.velocity_setpoint_radps:+.2f}rad/s")
    elif kind == "identity":
        print(f"[identity] id={env.request_id} build={env.identity.build_id}")
    elif kind == "response":
        verdict = "accepted" if env.response.accepted else f"REJECTED: {env.response.cause}"
        print(f"[reply] id={env.request_id} {verdict}")
    else:
        print(f"[?] id={env.request_id} payload={kind}")


def link_test(ser, pb2, next_id, payload_bytes, frame_count, timeout):
    """Run one throughput measurement: request, count frames, print a summary.

    Nothing prints during the run — terminal I/O would be part of what gets
    measured. Elapsed time spans the request write to the last frame received.
    """
    import serial  # pyserial, from the venv

    env = pb2.Envelope(request_id=next(next_id))
    env.link_test_request.payload_bytes = payload_bytes
    env.link_test_request.frame_count = frame_count
    started = time.monotonic()
    ser.write(frame(env.SerializeToString()))

    frames = 0
    payload_bytes_seen = 0
    wire_bytes_seen = 0
    crc_failures = 0
    decode_failures = 0
    short_frames = 0
    stale_frames = 0
    dropped = 0
    next_seq = 0
    last_frame = started
    buffer = bytearray()
    # Poll the driver buffer instead of blocking reads: a blocking read that
    # waits out its timeout lets the driver buffer overrun at these rates.
    ser.timeout = 0
    try:
        while True:
            chunk = ser.read(ser.in_waiting or 1)
            now = time.monotonic()
            # Consume what was just read before deciding to stop, or the last
            # read of the run is thrown away.
            buffer += chunk
            segments, buffer = split_frames(buffer)
            for segment in segments:
                payload = deframe(segment)
                if payload is None:
                    crc_failures += 1
                    continue
                env = pb2.Envelope()
                try:
                    env.ParseFromString(payload)
                except Exception:
                    decode_failures += 1
                    continue
                kind = env.WhichOneof("payload")
                if kind == "response" and not env.response.accepted:
                    print(f"[link-test] REJECTED: {env.response.cause}")
                    return
                if kind != "link_test_frame":
                    continue
                seq = env.link_test_frame.seq
                # seq never wraps (count <= 1e6), so a backwards one is a
                # leftover frame from an earlier run still in the buffer.
                if seq < next_seq:
                    stale_frames += 1
                    continue
                # Wire cost as the frame sat on the link: its COBS segment plus
                # the delimiter that ended it.
                wire_bytes_seen += len(segment) + 1
                size = len(env.link_test_frame.payload)
                payload_bytes_seen += size
                if size != payload_bytes:
                    short_frames += 1
                dropped += seq - next_seq
                next_seq = seq + 1
                frames += 1
                last_frame = now
            # Telemetry and log keep arriving throughout, so only frame silence
            # marks the end, which also bounds a board that never answers.
            if now - last_frame > LINK_TEST_QUIET_S:
                break
            if (timeout is not None) and (now - started > timeout):
                break
    except serial.SerialException as exc:
        print(f"[port] {ser.port} unavailable mid-test ({exc}); partial summary:")

    elapsed = max(last_frame - started, 1e-9)
    expected = frames + dropped
    loss = (100.0 * dropped / expected) if expected else 0.0
    # Every corrupt frame also shows up as a seq gap in the next good frame.
    corrupt = min(crc_failures + decode_failures, dropped)
    print(f"[link-test] requested {payload_bytes} B x {frame_count} frames")
    print(f"  frames        {frames}")
    print(f"  payload       {payload_bytes_seen} B")
    print(f"  wire          {wire_bytes_seen} B")
    print(f"  elapsed       {elapsed:.3f} s")
    print(f"  payload rate  {payload_bytes_seen / elapsed / 1000:.1f} kB/s")
    print(f"  wire rate     {wire_bytes_seen / elapsed / 1000:.1f} kB/s")
    print(f"  dropped       {dropped} ({loss:.2f}%), {corrupt} of them corrupt")
    print(f"  CRC failures  {crc_failures}")
    print(f"  decode fails  {decode_failures}")
    print(f"  short frames  {short_frames}")
    print(f"  stale frames  {stale_frames} (ignored)")


def session(ser, pb2, board_pb2, next_id, commands, watches=None, print_every=100):
    """Greet the board, then stream until the port dies (SerialException)."""
    for payload in ("identity_request", "ping"):
        env = pb2.Envelope(request_id=next(next_id))
        getattr(env, payload).SetInParent()
        ser.write(frame(env.SerializeToString()))

    # The watch list re-sends every session: it clears on port close and on a
    # board reset (fw~conn_trace_003), so a reconnect must re-install it.
    trace = {"watches": watches or [], "next_cycle": {}, "print_every": print_every}
    if watches:
        env = pb2.Envelope(request_id=next(next_id))
        env.watch_request.SetInParent()
        for addr, size, period, _fmt in watches:
            env.watch_request.watches.add(address=addr, size=size, period_cycles=period)
        print(f"[cmd] id={env.request_id} watch " +
              " ".join(f"0x{a:08X}:{s}:{p}cyc" for a, s, p, _ in watches))
        ser.write(frame(env.SerializeToString()))

    # One-shot board commands from the CLI (first session only — a board
    # reset must not silently re-apply an old command on reconnect).
    while commands:
        kind, value = commands.pop(0)
        env = pb2.Envelope(request_id=next(next_id))
        if kind == "set_mode":
            env.board_request.set_mode.mode = value
        elif kind == "set_velocity":
            env.board_request.set_velocity.velocity_radps = value
        elif kind == "clear_fault":
            env.board_request.clear_fault.SetInParent()
        elif kind == "read":
            env.read_request.address, env.read_request.size = value
        elif kind == "write":
            env.write_request.address = value[0]
            env.write_request.data = bytes(value[1])
        print(f"[cmd] id={env.request_id} {kind} {value if value is not None else ''}")
        ser.write(frame(env.SerializeToString()))

    buffer = bytearray()
    log_buffer = []
    while True:
        buffer += ser.read(4096)
        segments, buffer = split_frames(buffer)
        for segment in segments:
            payload = deframe(segment)
            if payload is None:
                print(f"[frame] discarded {len(segment)}-byte invalid segment")
                continue
            env = pb2.Envelope()
            try:
                env.ParseFromString(payload)
            except Exception:
                print(f"[frame] {len(payload)} bytes did not decode as Envelope")
                continue
            show(env, pb2, board_pb2, log_buffer, trace)


def run(port: str, baud: int, args):
    import serial  # pyserial, from the venv

    ensure_bindings()
    import board_pb2
    import shared_pb2 as pb2

    commands = []
    if args.set_mode is not None:
        mode = board_pb2.MODE_SIX_STEP_TRAP if args.set_mode == "six_step" else board_pb2.MODE_OFF
        commands.append(("set_mode", mode))
    if args.set_velocity is not None:
        commands.append(("set_velocity", args.set_velocity))
    if args.clear_fault:
        commands.append(("clear_fault", None))
    # Writes before reads, so a write -> read round trip works in one invocation.
    for spec in (args.write or []):
        try:
            addr_s, hexbytes = spec.split(":")
            commands.append(("write", (int(addr_s, 0), bytes.fromhex(hexbytes))))
        except ValueError:
            sys.exit(f"bad --write '{spec}' (expected ADDR:HEXBYTES, e.g. 0x20000110:00000000)")
    for spec in (args.read or []):
        try:
            addr, size = (int(p, 0) for p in spec.split(":"))
            commands.append(("read", (addr, size)))
        except ValueError:
            sys.exit(f"bad --read '{spec}' (expected ADDR:SIZE)")

    link_spec = None
    if args.link_test is not None:
        try:
            payload_bytes, frame_count = (int(part, 0) for part in args.link_test.split(":"))
        except ValueError:
            sys.exit(f"bad --link-test '{args.link_test}' (expected BYTES:COUNT, e.g. 256:20000)")
        link_spec = (payload_bytes, frame_count)

    watches = []
    for spec in (args.watch or []):
        try:
            parts = spec.split(":")
            if len(parts) > 4:
                raise ValueError
            addr, size, period = (int(part, 0) for part in parts[:3])
            fmt = parts[3] if len(parts) > 3 else "u"
            if (period not in (1, 20, 200)) or not (1 <= size <= 8):
                raise ValueError
            if fmt not in ("u", "f") or (fmt == "f" and size not in (4, 8)):
                raise ValueError
        except ValueError:
            sys.exit(f"bad --watch '{spec}' (expected ADDR:SIZE:PERIOD_CYCLES[:u|f]; "
                     "size 1..8; period 1, 20, or 200 PWM cycles; "
                     "f = decode as float, size 4 or 8)")
        watches.append((addr, size, period, fmt))

    next_id = itertools.count(1)

    # A link test is a one-shot measurement, not a stream: run it and exit,
    # rather than falling into the reconnect loop.
    if link_spec is not None:
        try:
            with serial.Serial(port, baud, timeout=0.05) as ser:
                grow_rx_buffer(ser)
                link_test(ser, pb2, next_id, link_spec[0], link_spec[1], args.timeout)
        except serial.SerialException as exc:
            print(f"[port] {port} unavailable ({exc})")
        return

    # Survive board resets: when the port dies (or isn't there yet), poll for
    # re-enumeration and start a fresh session — new greeting, empty buffer.
    sessions = 0
    waiting_announced = False
    while True:
        try:
            with serial.Serial(port, baud, timeout=0.05) as ser:
                grow_rx_buffer(ser)
                waiting_announced = False
                sessions += 1
                verb = "connected" if sessions == 1 else "reconnected"
                print(f"{verb} to {port}; streaming (Ctrl-C to stop)")
                session(ser, pb2, board_pb2, next_id, commands,
                        watches=watches, print_every=args.print_every)
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

    env = pb2.Envelope(request_id=8)
    env.watch_request.SetInParent()
    env.watch_request.watches.add(address=0x20000000, size=4, period_cycles=1)
    env.watch_request.watches.add(address=0x20000010, size=2, period_cycles=200)
    round_trip = pb2.Envelope()
    round_trip.ParseFromString(deframe(frame(env.SerializeToString())[1:-1]))
    assert round_trip.WhichOneof("payload") == "watch_request"
    assert [(w.address, w.size, w.period_cycles) for w in round_trip.watch_request.watches] == \
        [(0x20000000, 4, 1), (0x20000010, 2, 200)]

    # A Samples batch round-trips as records x count of the group's spans.
    env = pb2.Envelope()
    env.samples.first_cycle = 41
    env.samples.period_cycles = 20
    env.samples.count = 2
    env.samples.data = bytes([0x01, 0x02, 0x03, 0x04])
    round_trip = pb2.Envelope()
    round_trip.ParseFromString(deframe(frame(env.SerializeToString())[1:-1]))
    assert round_trip.request_id == 0
    assert round_trip.samples.first_cycle == 41 and round_trip.samples.count == 2
    assert round_trip.samples.period_cycles == 20
    assert len(round_trip.samples.data) == 4
    env = pb2.Envelope(request_id=9)
    env.link_test_request.payload_bytes = 256
    env.link_test_request.frame_count = 20000
    round_trip = pb2.Envelope()
    round_trip.ParseFromString(deframe(frame(env.SerializeToString())[1:-1]))
    assert round_trip.WhichOneof("payload") == "link_test_request"
    assert round_trip.link_test_request.payload_bytes == 256
    assert round_trip.link_test_request.frame_count == 20000

    # A run longer than 254 nonzero bytes splits into COBS 0xFF blocks.
    big = bytes((((i * 7) % 255) + 1) for i in range(300))
    assert cobs_decode(cobs_encode(big)) == big
    assert deframe(frame(big)[1:-1]) == big

    # ~270 B on the wire: the frame the throughput run counts, zeros included.
    env = pb2.Envelope()
    env.link_test_frame.seq = 4321
    env.link_test_frame.payload = bytes(range(256))
    wire = frame(env.SerializeToString())
    assert len(wire) > 260
    round_trip = pb2.Envelope()
    round_trip.ParseFromString(deframe(wire[1:-1]))
    assert round_trip.link_test_frame.seq == 4321
    assert round_trip.link_test_frame.payload == bytes(range(256))

    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port (e.g. COM5, /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by USB CDC")
    ap.add_argument("--set-mode", choices=["off", "six_step"], help="send a SetMode command on connect")
    ap.add_argument("--set-velocity", type=float, metavar="RAD_PER_S", help="send a SetVelocity command on connect")
    ap.add_argument("--clear-fault", action="store_true", help="send a ClearFault command on connect")
    ap.add_argument("--watch", action="append", metavar="ADDR:SIZE:PERIOD_CYCLES",
                    help="install a trace watch (repeatable, e.g. 0x20000000:4:20); "
                         "period is 1, 20, or 200 PWM cycles (50 us / 1 ms / 10 ms), "
                         "at most 4 watches at one cycle; "
                         "the list re-sends on every (re)connect")
    ap.add_argument("--read", action="append", metavar="ADDR:SIZE",
                    help="one-shot memory read on connect (repeatable)")
    ap.add_argument("--write", action="append", metavar="ADDR:HEXBYTES",
                    help="one-shot memory write on connect (repeatable; bytes land "
                         "in memory order, e.g. 0x20000110:00000000)")
    ap.add_argument("--link-test", metavar="BYTES:COUNT",
                    help="measure link throughput: stream COUNT frames of BYTES payload "
                         "(1..256 : 1..1000000) and print one summary, then exit")
    ap.add_argument("--timeout", type=float, metavar="SECONDS",
                    help="give up on a --link-test run after this long")
    ap.add_argument("--print-every", type=int, default=100, metavar="N",
                    help="print every Nth sample record, counted across all groups "
                         "(default 100); gaps always print")
    ap.add_argument("--selftest", action="store_true", help="verify framing + bindings offline")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())
    if not args.port:
        ap.error("--port is required (or use --selftest)")
    if (args.link_test is not None) and any((args.watch, args.read, args.write,
                                             args.set_mode, args.clear_fault,
                                             args.set_velocity is not None)):
        ap.error("--link-test is a standalone measurement; run it without "
                 "--watch/--read/--write/--set-mode/--set-velocity/--clear-fault")
    try:
        run(args.port, args.baud, args)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
