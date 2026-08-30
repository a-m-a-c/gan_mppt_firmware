#!/usr/bin/env python3
"""The one holder of the serial port, with subscribers fanned out from it.

Only one process can hold the COM port, which is why live_plot.py had to carry
its own command prompt. A server changes that constraint rather than working
around it: this process holds the port, and any number of browser tabs, plus a
Recorder, subscribe to the same decoded stream.

One reader thread decodes with console.StreamParser and calls every subscriber
with (name, value, t). Subscribers must return quickly - the stream is ~5000
packets a second and the thread does nothing else.

ReplayLink presents the same surface from a capture CSV, so the whole GUI can
be exercised without the board. That is how the plotting is verified.
"""

from __future__ import annotations

import csv
import sys
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import console  # noqa: E402  - protocol definitions, single source


def available_ports() -> list[dict]:
    return [{"device": p.device, "description": p.description or "",
             "manufacturer": p.manufacturer or ""} for p in list_ports.comports()]


class LinkError(RuntimeError):
    pass


class BaseLink:
    """Subscriber list, clock and lifecycle flags. A source fills in the rest."""

    kind = "link"

    def __init__(self) -> None:
        self._thread: threading.Thread | None = None
        self._running = False
        self._lock = threading.Lock()
        self._subscribers: list = []
        self._t0 = time.monotonic()
        self.port: str | None = None
        self.error: str | None = None

    @property
    def connected(self) -> bool:
        return self._running

    def subscribe(self, fn) -> None:
        with self._lock:
            self._subscribers.append(fn)

    def unsubscribe(self, fn) -> None:
        with self._lock:
            if fn in self._subscribers:
                self._subscribers.remove(fn)

    def _dispatch(self, name: str, value: int, t: float) -> None:
        with self._lock:
            subscribers = list(self._subscribers)
        for fn in subscribers:
            fn(name, value, t)

    def clock(self) -> float:
        """Seconds since this connection opened. The recorder's t_host."""
        return time.monotonic() - self._t0

    def _start(self, target) -> None:
        self._t0 = time.monotonic()
        self._running = True
        self._thread = threading.Thread(target=target, daemon=True)
        self._thread.start()

    def _stop_thread(self) -> None:
        self._running = False
        thread, self._thread = self._thread, None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1.0)


class SerialLink(BaseLink):
    """Open/close is explicit; the object outlives any one connection."""

    kind = "serial"

    def __init__(self) -> None:
        super().__init__()
        self._ser: serial.Serial | None = None
        self._parser = console.StreamParser()

    @property
    def connected(self) -> bool:
        return self._ser is not None and self._running

    def open(self, port: str | None = None) -> str:
        if self.connected:
            raise LinkError(f"already connected to {self.port}")
        chosen = port or console.pick_port()
        if chosen is None:
            raise LinkError("no serial port found")
        try:
            ser = serial.Serial(chosen, console.BAUD, timeout=0.05)
        except serial.SerialException as exc:
            raise LinkError(f"could not open {chosen}: {exc}") from exc
        self._ser = ser
        self.port = chosen
        self.error = None
        self._parser = console.StreamParser()   # a new connection starts unaligned
        self._start(self._read_loop)
        return chosen

    def close(self) -> None:
        self._stop_thread()
        if self._ser is not None:
            try:
                self._ser.close()
            except serial.SerialException:
                pass
        self._ser = None

    def _read_loop(self) -> None:
        ser = self._ser
        while self._running and ser is not None:
            try:
                # Whatever is buffered, not a fixed count: a fixed read can
                # never fill at this rate, so it waits out the timeout and
                # stamps a whole batch with one arrival time.
                data = ser.read(max(1, ser.in_waiting))
            except (serial.SerialException, OSError, TypeError) as exc:
                self.error = str(exc)
                self._running = False
                return
            if not data:
                continue
            now = time.monotonic() - self._t0
            for name, value in self._parser.feed(data):
                self._dispatch(name, value, now)

    def send(self, verb: str) -> bytes:
        if verb not in console.OPCODES:
            raise LinkError(f"unknown command: {verb}")
        return self._write(console.encode(console.OPCODES[verb]))

    def send_raw(self, op: int, payload: bytes = b"") -> bytes:
        # console.encode refuses a payload over TRANSPORT_MAX_PAYLOAD; serial.c
        # latches SERIAL_STATE_ERROR on one and that costs a power cycle.
        try:
            frame = console.encode(op, payload)
        except ValueError as exc:
            raise LinkError(str(exc)) from exc
        return self._write(frame)

    def _write(self, frame: bytes) -> bytes:
        ser = self._ser
        if ser is None or not self._running:
            raise LinkError("not connected")
        try:
            ser.write(frame)
        except serial.SerialException as exc:
            raise LinkError(f"write failed: {exc}") from exc
        return frame

    def stats(self) -> dict:
        return {"frames": self._parser.frames, "resyncs": self._parser.resyncs,
                "uptime": self.clock() if self.connected else 0.0}


class ReplayLink(BaseLink):
    """A capture CSV played back through the same subscriber path.

    The file's own t_s is the clock, so a replay reproduces the timing the
    board had. Commands are accepted and discarded: there is nothing to command,
    but the UI should behave the same either way.
    """

    kind = "replay"

    # The order Src/app/stream.c sends them, so STREAM_LAST still closes a set.
    ORDER = ("vbus_mv", "duty", "vin_mv", "iin_ma", "vin_target_mv", "flags")

    def __init__(self, path: Path, speed: float = 1.0, loop: bool = False) -> None:
        super().__init__()
        self.path = Path(path)
        self.speed = speed
        self.loop = loop
        self.rows: list[dict] = []
        self.sent = 0

    def open(self, port: str | None = None) -> str:
        if self._running:
            raise LinkError("replay already running")
        if not self.path.exists():
            raise LinkError(f"{self.path} not found")
        with self.path.open(newline="") as fh:
            self.rows = list(csv.DictReader(fh))
        if not self.rows:
            raise LinkError(f"{self.path.name} has no rows")
        self.port = f"replay:{self.path.name}"
        self.error = None
        self.sent = 0
        self._start(self._play_loop)
        return self.port

    def close(self) -> None:
        self._stop_thread()

    def _play_loop(self) -> None:
        while self._running:
            wall0 = time.monotonic()
            t0 = None
            for raw in self.rows:
                if not self._running:
                    return
                try:
                    t = float(raw["t_s"])
                except (KeyError, ValueError):
                    continue
                if t0 is None:
                    t0 = t
                if self.speed > 0:
                    behind = (t - t0) / self.speed - (time.monotonic() - wall0)
                    if behind > 0:
                        time.sleep(behind)
                now = time.monotonic() - self._t0
                for name in self.ORDER:
                    value = raw.get(name)
                    if value not in ("", None):
                        self.sent += 1
                        self._dispatch(name, int(value), now)
            if not self.loop:
                self._running = False
                return

    def send(self, verb: str) -> bytes:
        if verb not in console.OPCODES:
            raise LinkError(f"unknown command: {verb}")
        return console.encode(console.OPCODES[verb])

    def send_raw(self, op: int, payload: bytes = b"") -> bytes:
        try:
            return console.encode(op, payload)
        except ValueError as exc:
            raise LinkError(str(exc)) from exc

    def stats(self) -> dict:
        return {"frames": self.sent, "resyncs": 0,
                "uptime": self.clock() if self.connected else 0.0}
