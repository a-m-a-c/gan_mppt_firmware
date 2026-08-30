#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""Serial console for the GaN MPPT board.

Sends system commands and decodes the telemetry stream.

    uv run tools/console.py            # auto-detect the port
    uv run tools/console.py --list     # show candidate ports
    uv run tools/console.py --port COM7
    uv run tools/console.py --watch    # start with telemetry printing

The two directions do NOT share a frame format. Both are taken from the
firmware, which is the source of truth:

  host -> board   [op][size][data...][crc]     Src/drivers/serial.c
  board -> host   [id][size][data...]          Src/app/stream.c

Payloads are little endian.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time

import serial
from serial.tools import list_ports

BAUD = 921600  # Src/usart.c, huart5.Init.BaudRate

# --------------------------------------------------------------------------
# Outgoing: opcodes from serial_opcode_t in Src/app/command.c
# --------------------------------------------------------------------------
OPCODES = {
    "reset": 0x01,
    "clearfault": 0x02,
    "stop": 0x03,
    "mppt": 0x04,
    "cv": 0x05,
    "chmppt": 0x06,
    "ivsweep": 0x07,
}

# serial.c latches SERIAL_STATE_ERROR on size > TRANSPORT_MAX_PAYLOAD, which
# kills the link until the board is power-cycled. Inc/drivers/transport.h.
MAX_PAYLOAD = 8

# The firmware does not check this yet - Src/drivers/serial.c accepts the frame
# in SERIAL_STATE_DECODE_CRC without looking. 0xCC is what the format comment
# says the host sends. Swap for a real CRC-8 when the firmware checks one.
CRC_STUB = 0xCC

# --------------------------------------------------------------------------
# Incoming: ids and widths from Src/app/stream.c
# --------------------------------------------------------------------------
# Src/app/stream.c, STREAM_PERIOD_MS. Printing faster than this only repeats
# values, so it is the floor for `rate`.
STREAM_PERIOD_MS = 1

# id -> (name, width, signed). Width doubles as the resync check, so it has to
# match the firmware exactly.
STREAM = {
    0x60: ("vbus_mv", 4, False),
    0x61: ("duty", 2, False),
    0x62: ("flags", 1, False),
    0x63: ("vin_mv", 4, False),
    0x64: ("iin_ma", 4, True),
    0x65: ("vin_target_mv", 2, False),
}

# The board sends a set in this order; the first and last are what the host
# uses to bracket one set. Changing the order in stream.c changes this - a new
# packet goes BEFORE flags, or every parser here loses its set boundary.
STREAM_FIRST = "vbus_mv"
STREAM_LAST = "flags"


def encode(op: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} exceeds TRANSPORT_MAX_PAYLOAD ({MAX_PAYLOAD})")
    return bytes([op, len(payload)]) + payload + bytes([CRC_STUB])


class StreamParser:
    """Length-prefixed with no delimiter, so alignment has to be earned.

    Connecting mid-stream is the normal case, not an edge case. Every id and
    its width are known, so an unknown id or a width that disagrees means we
    are misaligned: drop one byte and try again.
    """

    def __init__(self) -> None:
        self.buf = bytearray()
        self.resyncs = 0
        self.frames = 0

    def feed(self, data: bytes) -> list[tuple[str, int]]:
        self.buf += data
        out: list[tuple[str, int]] = []
        while True:
            if len(self.buf) < 2:
                return out
            ident, size = self.buf[0], self.buf[1]
            known = STREAM.get(ident)
            if known is None or known[1] != size:
                del self.buf[0]
                self.resyncs += 1
                continue
            if len(self.buf) < 2 + size:
                return out
            name, _, signed = known
            value = int.from_bytes(self.buf[2 : 2 + size], "little", signed=signed)
            del self.buf[: 2 + size]
            self.frames += 1
            out.append((name, value))


class Board:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, BAUD, timeout=0.05)
        self.parser = StreamParser()
        self.latest: dict[str, int] = {}
        self.lock = threading.Lock()
        self.running = True
        self.watch = False
        self.watch_interval = 0.5  # seconds between watch lines
        self._last_print = 0.0
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self) -> None:
        while self.running:
            try:
                data = self.ser.read(max(1, self.ser.in_waiting))
            except serial.SerialException as exc:
                print(f"\n[port closed: {exc}]")
                self.running = False
                return
            if not data:
                continue
            for name, value in self.parser.feed(data):
                with self.lock:
                    self.latest[name] = value
            if self.watch and (time.monotonic() - self._last_print) >= self.watch_interval:
                self._last_print = time.monotonic()
                print("\r" + self.summary())
                print("> ", end="", flush=True)

    def summary(self) -> str:
        with self.lock:
            latest = dict(self.latest)
        if "vbus_mv" not in latest:
            return "no telemetry yet"
        vbus = latest["vbus_mv"] / 1000.0
        vin = latest.get("vin_mv", 0) / 1000.0
        iin = latest.get("iin_ma", 0) / 1000.0
        duty = latest.get("duty", 0)
        flags = latest.get("flags", 0)
        return (
            f"vin {vin:6.3f} V  iin {iin:7.3f} A  pin {vin * iin:7.2f} W  "
            f"vbus {vbus:6.2f} V  duty {duty:4d}/1000  flags 0x{flags:02X}"
            f"  [{self.parser.frames} frames, {self.parser.resyncs} resynced]"
        )

    def send(self, op: int, payload: bytes = b"") -> None:
        frame = encode(op, payload)
        self.ser.write(frame)
        print(f"  sent {frame.hex(' ')}")

    def close(self) -> None:
        self.running = False
        self.reader.join(timeout=1.0)
        self.ser.close()


def pick_port() -> str | None:
    ports = list(list_ports.comports())
    if not ports:
        return None
    for p in ports:
        blurb = f"{p.description} {p.manufacturer or ''}".lower()
        if any(k in blurb for k in ("usb", "cp210", "ch340", "ftdi", "st-link", "stlink")):
            return p.device
    return ports[0].device


HELP = """commands
  reset | clearfault | stop                        send a system command
  mppt | cv | chmppt | ivsweep                     run a mode
  raw <op-hex> [byte-hex ...]                      send an arbitrary frame
  watch                                            toggle telemetry printing
  rate <interval_ms>                               set the watch print interval
  status                                           print the latest values once
  ports                                            list serial ports
  help | quit
"""


def repl(board: Board) -> None:
    print(HELP)
    while board.running:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        parts = line.split()
        verb, args = parts[0].lower(), parts[1:]

        if verb in ("quit", "exit", "q"):
            return
        if verb == "help":
            print(HELP)
        elif verb == "watch":
            board.watch = not board.watch
            print(f"  telemetry {'on' if board.watch else 'off'}"
                  f" at {board.watch_interval * 1000:.0f} ms")
        elif verb == "rate":
            try:
                ms = int(args[0])
            except (IndexError, ValueError):
                print(f"  usage: rate <interval_ms>   (now {board.watch_interval * 1000:.0f} ms)")
                continue
            if ms < STREAM_PERIOD_MS:
                print(f"  floor is {STREAM_PERIOD_MS} ms - the board only sends that often")
                ms = STREAM_PERIOD_MS
            board.watch_interval = ms / 1000.0
            print(f"  watch interval {ms} ms")
        elif verb == "status":
            print("  " + board.summary())
        elif verb == "ports":
            for p in list_ports.comports():
                print(f"  {p.device:10s} {p.description}")
        elif verb == "raw":
            try:
                op = int(args[0], 16)
                payload = bytes(int(a, 16) for a in args[1:])
                board.send(op, payload)
            except (IndexError, ValueError) as exc:
                print(f"  usage: raw <op-hex> [byte-hex ...]  ({exc})")
        elif verb in OPCODES:
            board.send(OPCODES[verb])
        else:
            print(f"  unknown: {verb}  (try help)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port; auto-detected if omitted")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--watch", action="store_true", help="start with telemetry printing on")
    args = ap.parse_args()

    if args.list:
        found = list(list_ports.comports())
        if not found:
            print("no serial ports found")
        for p in found:
            print(f"{p.device:10s} {p.description}")
        return 0

    port = args.port or pick_port()
    if port is None:
        print("no serial port found; try --list", file=sys.stderr)
        return 1

    try:
        board = Board(port)
    except serial.SerialException as exc:
        print(f"could not open {port}: {exc}", file=sys.stderr)
        return 1

    print(f"connected to {port} at {BAUD}")
    board.watch = args.watch
    try:
        repl(board)
    finally:
        board.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
