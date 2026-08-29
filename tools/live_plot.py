#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "matplotlib>=3.8", "numpy>=1.26"]
# ///
"""Live V-I scope: operating points accumulate on the curve as they arrive.

    uv run tools/live_plot.py                       # watch the board
    uv run tools/live_plot.py --persist 30          # shorter tail
    uv run tools/live_plot.py --replay stream_plot.csv --speed 4

Each telemetry sample is one point on the V-I plane, fading out over --persist
seconds, so an MPPT algorithm draws its own search: a converging spiral onto
the knee, or a limit cycle hunting around it. The green star is the highest
power seen inside the persistence window and the dashed hyperbola is that power
level, so "is the tracker sitting on the best point it has found" is a question
you answer by looking.

Commands are typed at a prompt in the terminal - the same verbs console.py
takes, read from console.OPCODES so the two cannot drift. They live here because
only one process can hold the COM port: watching live and commanding cannot be
split across two tools.

--replay feeds a stream_plot.csv through the same path instead of the port,
using the file's own t_s as the clock.

Protocol constants come from console.py, which takes them from the firmware.
"""

from __future__ import annotations

import argparse
import csv
import queue
import sys
import threading
import time
from collections import deque
from pathlib import Path

import numpy as np
import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import console  # noqa: E402  - protocol definitions, single source

import matplotlib  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.animation import FuncAnimation  # noqa: E402

# 1 kHz of sets is far more than a screen can show. Telemetry only moves at
# 25 Hz anyway; this is for vbus and duty, which do change every set.
TIMESERIES_DECIMATE = 20

HELP = """commands
  mppt | cv | chmppt | ivsweep      run a mode
  stop | reset | clearfault         send a system command
  raw <op-hex> [byte-hex ...]       send an arbitrary frame
  status                            print the latest values once
  clear                             empty the V-I cloud
  persist <seconds>                 change how long a point stays
  window <seconds>                  change the time-series span
  save [path]                       write a PNG of the window
  ports                             list serial ports
  help | quit
"""

VIN_C, IIN_C, PIN_C, VBUS_C, DUTY_C, MPP_C = (
    "#ff7f0e", "#9467bd", "#2ca02c", "#1f77b4", "#d62728", "#2ca02c")


class Live:
    """Everything the animation draws. Written by a source thread, read by the
    draw callback, so every deque is touched under the lock."""

    def __init__(self, persist: float, window: float) -> None:
        self.persist = persist
        self.window = window
        self.lock = threading.Lock()
        self.iv: deque[tuple[float, float, float, float]] = deque()   # t, i, v, p
        self.ts: deque[tuple[float, float, float, float, float, int]] = deque()
        self.latest: dict[str, int] = {}
        self.last_sample: tuple[int, int] | None = None
        self.sets = 0
        self.dropped = 0    # sets discarded for a clear telemetry-valid flag
        self.now = 0.0

    def ingest(self, name: str, value: int, t: float) -> None:
        """One decoded packet. A set is complete at STREAM_LAST."""
        with self.lock:
            self.latest[name] = value
            self.now = t
            if name != console.STREAM_LAST:
                return

            self.sets += 1
            vin_mv = self.latest.get("vin_mv")
            iin_ma = self.latest.get("iin_ma")
            vbus_mv = self.latest.get("vbus_mv")
            duty = self.latest.get("duty")
            if None in (vin_mv, iin_ma, vbus_mv, duty):
                return          # partial set, normal when connecting mid-stream
            if not (self.latest.get("flags", 0) & 0x01):
                self.dropped += 1   # channel_telem.c did not complete its sweep
                return

            v, i, vbus = vin_mv / 1000.0, iin_ma / 1000.0, vbus_mv / 1000.0

            # A V-I point only when the measurement actually changed. Repeating
            # one telemetry sample 40 times would stack 40 dots on one spot and
            # make a stale reading look like a confident one.
            sample = (vin_mv, iin_ma)
            if sample != self.last_sample:
                self.last_sample = sample
                self.iv.append((t, i, v, v * i))

            if self.sets % TIMESERIES_DECIMATE == 0:
                self.ts.append((t, v, i, v * i, vbus, duty))

            self._prune(t)

    def _prune(self, now: float) -> None:
        while self.iv and self.iv[0][0] < now - self.persist:
            self.iv.popleft()
        while self.ts and self.ts[0][0] < now - self.window:
            self.ts.popleft()

    def snapshot(self):
        with self.lock:
            return list(self.iv), list(self.ts), dict(self.latest), self.now

    # The REPL runs on its own thread, so everything it touches goes through the
    # same lock the ingest path uses.
    def clear(self) -> None:
        with self.lock:
            self.iv.clear()
            self.ts.clear()

    def set_persist(self, seconds: float) -> None:
        with self.lock:
            self.persist = seconds   # takes effect on the next set's prune

    def set_window(self, seconds: float) -> None:
        with self.lock:
            self.window = seconds

    def status_line(self, source) -> str:
        with self.lock:
            latest, held = dict(self.latest), len(self.iv)
        vin = latest.get("vin_mv", 0) / 1000.0
        iin = latest.get("iin_ma", 0) / 1000.0
        return (
            f"vin {vin:6.3f} V   iin {iin:7.3f} A   pin {vin * iin:7.2f} W   "
            f"vbus {latest.get('vbus_mv', 0) / 1000.0:6.3f} V   "
            f"duty {latest.get('duty', 0):4d}   "
            f"flags 0x{latest.get('flags', 0):02X}" + "\n"
            f"{self.sets} sets, {source.stats()}, {self.dropped} dropped, "
            f"{held} points held")


class SerialSource:
    def __init__(self, port: str, live: Live) -> None:
        self.ser = serial.Serial(port, console.BAUD, timeout=0.05)
        self.parser = console.StreamParser()
        self.live = live
        self.running = True
        self.t0 = time.monotonic()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self) -> None:
        while self.running:
            try:
                data = self.ser.read(max(1, self.ser.in_waiting))
            except serial.SerialException as exc:
                print(f"[port closed: {exc}]")
                self.running = False
                return
            if not data:
                continue
            t = time.monotonic() - self.t0
            for name, value in self.parser.feed(data):
                self.live.ingest(name, value, t)

    def send(self, verb: str) -> None:
        frame = console.encode(console.OPCODES[verb])
        self.ser.write(frame)
        print(f"  sent {verb:10s} {frame.hex(' ')}")

    def send_raw(self, op: int, payload: bytes = b"") -> None:
        # console.encode refuses a payload over TRANSPORT_MAX_PAYLOAD; serial.c
        # latches on one and that costs a power cycle.
        frame = console.encode(op, payload)
        self.ser.write(frame)
        print(f"  sent raw    {frame.hex(' ')}")

    def stats(self) -> str:
        return f"{self.parser.frames} frames, {self.parser.resyncs} resynced"

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=1.0)
        self.ser.close()


class ReplaySource:
    """Feeds a capture through the same ingest path, on the file's own clock."""

    def __init__(self, path: Path, live: Live, speed: float) -> None:
        self.live = live
        self.speed = speed
        self.rows = self._read(path)
        self.running = True
        self.done = threading.Event()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    @staticmethod
    def _read(path: Path) -> list[dict]:
        with path.open(newline="") as fh:
            return list(csv.DictReader(fh))

    def _loop(self) -> None:
        wall0 = time.monotonic()
        t0 = None
        for raw in self.rows:
            if not self.running:
                break
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
            # Same order stream.c sends them, so STREAM_LAST still closes the set.
            for name in ("vbus_mv", "vin_mv", "iin_ma", "duty", "flags"):
                value = raw.get(name)
                if value not in ("", None):
                    self.live.ingest(name, int(value), t)
        self.done.set()

    def send(self, verb: str) -> None:
        print(f"  (replay: ignoring {verb})")

    def send_raw(self, op: int, payload: bytes = b"") -> None:
        print(f"  (replay: ignoring raw 0x{op:02X})")

    def stats(self) -> str:
        return f"replay x{self.speed:g}" if self.speed else "replay (fast)"

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=1.0)


def build_figure(live: Live, source):
    fig = plt.figure(figsize=(14, 8))
    grid = fig.add_gridspec(4, 3, width_ratios=[1.7, 1, 1], hspace=0.35, wspace=0.30)

    ax_iv = fig.add_subplot(grid[:, 0])
    ax_v = fig.add_subplot(grid[0, 1:])
    ax_i = fig.add_subplot(grid[1, 1:], sharex=ax_v)
    ax_p = fig.add_subplot(grid[2, 1:], sharex=ax_v)
    ax_d = fig.add_subplot(grid[3, 1:], sharex=ax_v)

    ax_iv.set_xlabel("iin (A)")
    ax_iv.set_ylabel("vin (V)")
    ax_iv.set_title(f"operating points, {live.persist:g} s persistence", fontsize=10)
    ax_iv.grid(alpha=0.3)

    art = {
        "cloud": ax_iv.scatter([], [], s=16, linewidths=0),
        "tail": ax_iv.plot([], [], lw=1.0, color=VIN_C, alpha=0.45)[0],
        "current": ax_iv.plot([], [], "o", ms=11, color=VIN_C,
                              mec="#333333", mew=1.0)[0],
        "best": ax_iv.plot([], [], "*", ms=18, color=MPP_C)[0],
        "iso": ax_iv.plot([], [], ls="--", lw=1.0, color=MPP_C, alpha=0.55)[0],
        "vin": ax_v.plot([], [], lw=1.2, color=VIN_C, label="vin")[0],
        "vbus": ax_v.plot([], [], lw=1.2, color=VBUS_C, label="vbus")[0],
        "iin": ax_i.plot([], [], lw=1.2, color=IIN_C)[0],
        "pin": ax_p.plot([], [], lw=1.2, color=PIN_C)[0],
        "duty": ax_d.plot([], [], lw=1.2, color=DUTY_C, drawstyle="steps-post")[0],
    }

    ax_v.set_ylabel("volts")
    ax_v.legend(loc="upper left", fontsize=8)
    ax_i.set_ylabel("iin (A)")
    ax_p.set_ylabel("pin (W)")
    ax_d.set_ylabel("duty")
    ax_d.set_xlabel("time (s)")
    for ax in (ax_v, ax_i, ax_p, ax_d):
        ax.grid(alpha=0.3)

    status = fig.text(0.01, 0.985, "waiting for telemetry", fontsize=9,
                      family="monospace", va="top")
    fig.text(0.99, 0.985, "commands: type 'help' in the terminal", fontsize=8,
             color="#777777", ha="right", va="top")

    return fig, (ax_iv, ax_v, ax_i, ax_p, ax_d), art, status


def make_update(live: Live, source, axes, art, status, requests=None):
    ax_iv, ax_v, ax_i, ax_p, ax_d = axes
    base = np.array(matplotlib.colors.to_rgba(VIN_C))

    def update(_frame=None):
        iv, ts, latest, now = live.snapshot()

        if iv:
            pts = np.array([(p[1], p[2]) for p in iv])
            ages = now - np.array([p[0] for p in iv])
            # Linear fade to a floor rather than to zero: the oldest points are
            # the shape of the curve and should stay legible while the newest
            # ones are obviously the live end.
            alpha = np.clip(1.0 - ages / live.persist, 0.06, 1.0)
            colours = np.tile(base, (len(iv), 1))
            colours[:, 3] = alpha
            art["cloud"].set_offsets(pts)
            art["cloud"].set_facecolors(colours)

            tail = pts[-60:]
            art["tail"].set_data(tail[:, 0], tail[:, 1])
            art["current"].set_data(pts[-1:, 0], pts[-1:, 1])

            powers = np.array([p[3] for p in iv])
            best = int(powers.argmax())
            art["best"].set_data([iv[best][1]], [iv[best][2]])

            xmax = max(pts[:, 0].max() * 1.15, 0.1)
            ymax = max(pts[:, 1].max() * 1.15, 0.1)
            ax_iv.set_xlim(0, xmax)
            ax_iv.set_ylim(0, ymax)

            # Constant-power hyperbola through the best point: v = P/i. Anything
            # above this line is a better operating point than the tracker has
            # found, which is the whole question being asked. Started at P/ymax
            # so the asymptote does not climb out of the axes and dominate them.
            span = np.linspace(max(powers[best] / ymax, 1e-3), xmax, 100)
            art["iso"].set_data(span, powers[best] / span)

        if ts:
            t = [r[0] for r in ts]
            art["vin"].set_data(t, [r[1] for r in ts])
            art["vbus"].set_data(t, [r[4] for r in ts])
            art["iin"].set_data(t, [r[2] for r in ts])
            art["pin"].set_data(t, [r[3] for r in ts])
            art["duty"].set_data(t, [r[5] for r in ts])
            ax_v.set_xlim(max(0.0, now - live.window), max(now, live.window))
            for ax, series in ((ax_v, [r[1] for r in ts] + [r[4] for r in ts]),
                               (ax_i, [r[2] for r in ts]),
                               (ax_p, [r[3] for r in ts]),
                               (ax_d, [r[5] for r in ts])):
                top = max(series) if series else 1.0
                ax.set_ylim(min(0.0, min(series)), max(top * 1.15, 0.1))

        status.set_text(live.status_line(source))

        # Anything that touches the figure happens here, on the GUI thread. Tk
        # is not safe to call into from the REPL thread, so the REPL queues.
        while requests is not None:
            try:
                what, arg = requests.get_nowait()
            except queue.Empty:
                break
            if what == "save":
                status.figure.savefig(arg, dpi=110)
                print(f"  wrote {arg}")
            elif what == "quit":
                plt.close(status.figure)

        return tuple(art.values())

    return update


def repl(live: Live, source, requests: queue.SimpleQueue) -> None:
    """Same verbs console.py takes, on the same opcode table.

    Runs on its own thread because plt.show() owns the main one. Sending from
    here while the source thread reads is the one-writer/one-reader split
    pyserial supports, and the same split console.py already runs.
    """
    print(HELP)
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            requests.put(("quit", None))
            return
        if not line:
            continue
        parts = line.split()
        verb, args = parts[0].lower(), parts[1:]

        if verb in ("quit", "exit", "q"):
            requests.put(("quit", None))
            return
        if verb == "help":
            print(HELP)
        elif verb in console.OPCODES:
            source.send(verb)
        elif verb == "raw":
            try:
                source.send_raw(int(args[0], 16), bytes(int(a, 16) for a in args[1:]))
            except (IndexError, ValueError) as exc:
                print(f"  usage: raw <op-hex> [byte-hex ...]  ({exc})")
        elif verb == "status":
            print("  " + live.status_line(source).replace("\n", "\n  "))
        elif verb == "clear":
            live.clear()
            print("  cloud cleared")
        elif verb in ("persist", "window"):
            try:
                seconds = float(args[0])
                if seconds <= 0:
                    raise ValueError("must be positive")
            except (IndexError, ValueError) as exc:
                print(f"  usage: {verb} <seconds>  ({exc})")
                continue
            (live.set_persist if verb == "persist" else live.set_window)(seconds)
            print(f"  {verb} {seconds:g} s")
        elif verb == "save":
            requests.put(("save", Path(args[0]) if args else Path("live_plot.png")))
        elif verb == "ports":
            for port in console.list_ports.comports():
                print(f"  {port.device:10s} {port.description}")
        else:
            print(f"  unknown: {verb}  (try help)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port; auto-detected if omitted")
    ap.add_argument("--persist", type=float, default=100.0,
                    help="seconds a V-I point stays on the plot (default 100)")
    ap.add_argument("--window", type=float, default=30.0,
                    help="seconds shown on the time series (default 30)")
    ap.add_argument("--replay", type=Path, help="feed a stream_plot.csv instead of the port")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="replay speed multiplier; 0 is as fast as possible")
    ap.add_argument("--save", type=Path,
                    help="render the replay headless and write a PNG instead of a window")
    args = ap.parse_args()

    live = Live(args.persist, args.window)

    if args.replay:
        if not args.replay.exists():
            print(f"{args.replay} not found", file=sys.stderr)
            return 1
        if args.save:
            matplotlib.use("Agg")
        source = ReplaySource(args.replay, live, 0.0 if args.save else args.speed)
    else:
        if args.save:
            print("--save only applies to --replay", file=sys.stderr)
            return 2
        port = args.port or console.pick_port()
        if port is None:
            print("no serial port found; try: uv run tools/console.py --list",
                  file=sys.stderr)
            return 1
        try:
            source = SerialSource(port, live)
        except serial.SerialException as exc:
            print(f"could not open {port}: {exc}", file=sys.stderr)
            return 1
        print(f"connected to {port} at {console.BAUD}")

    requests: queue.SimpleQueue = queue.SimpleQueue()
    fig, axes, art, status = build_figure(live, source)
    update = make_update(live, source, axes, art, status, requests)

    if args.save:
        source.done.wait(timeout=60)
        update()
        fig.savefig(args.save, dpi=110)
        source.close()
        print(f"wrote {args.save}")
        return 0

    if matplotlib.get_backend().lower() == "agg":
        print("matplotlib has no interactive backend here - install tkinter, or "
              "use --replay with --save", file=sys.stderr)
        source.close()
        return 1

    threading.Thread(target=repl, args=(live, source, requests), daemon=True).start()

    anim = FuncAnimation(fig, update, interval=100, blit=False,
                         cache_frame_data=False)
    fig._live_anim = anim  # keep a reference; FuncAnimation dies if collected
    try:
        plt.show()
    finally:
        source.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
