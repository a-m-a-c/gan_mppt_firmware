#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "matplotlib>=3.8"]
# ///
"""Scripted bench run: drive the board through a sequence and plot the result.

    uv run tools/plotter.py --mode cv --start 2 --end 5 --length 8

Captures telemetry for --length seconds, sending the mode command at --start
and STOP at --end. Writes stream_plot.csv and stream_plot.svg to the repo root,
with the command instants marked.

Protocol constants come from console.py, which takes them from the firmware.
"""

from __future__ import annotations

import argparse
import csv
import sys
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import console  # noqa: E402  - protocol definitions, single source

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
CSV_PATH = REPO_ROOT / "stream_plot.csv"
SVG_PATH = REPO_ROOT / "stream_plot.svg"

# Modes that can be started. STOP is sent automatically at --end.
MODES = {"cv": "cv", "mppt": "mppt", "chmppt": "chmppt"}


class Capture:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, console.BAUD, timeout=0.05)
        self.parser = console.StreamParser()
        # (t_board, t_host, vbus_mv, duty, flags), one row per completed set
        self.rows: list[tuple[float, float, int | None, int | None, int | None]] = []
        self.events: list[tuple[float, str]] = []
        self.latest: dict[str, int] = {}
        self.ticks = -1   # sets seen; the board emits one every STREAM_PERIOD_MS
        self.partial = 0  # sets that did not deliver all three packets
        self.complete = True
        self.recording = True
        self.lock = threading.Lock()
        self.running = True
        self.t0 = time.monotonic()
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()

    def _read_loop(self) -> None:
        while self.running:
            try:
                # Whatever is buffered, not a fixed count - read(256) can never
                # fill at 1300 B/s, so it always waited out the timeout and
                # stamped a whole 50 ms batch with a single arrival time.
                data = self.ser.read(max(1, self.ser.in_waiting))
            except serial.SerialException:
                self.running = False
                return
            if not data:
                continue
            now = time.monotonic() - self.t0
            for name, value in self.parser.feed(data):
                if not self.recording:
                    continue
                with self.lock:
                    if name == "vbus_mv":  # first packet of a set
                        if self.ticks >= 0 and not self.complete:
                            self.partial += 1
                        self.ticks += 1
                        self.complete = False
                    self.latest[name] = value
                    if name == "flags":  # last packet of a set
                        self.complete = True
                        self.rows.append((
                            self.ticks * console.STREAM_PERIOD_MS / 1000.0,
                            now,
                            self.latest.get("vbus_mv"),
                            self.latest.get("duty"),
                            self.latest.get("flags"),
                        ))

    def send(self, verb: str) -> None:
        frame = console.encode(console.OPCODES[verb])
        self.ser.write(frame)
        t = time.monotonic() - self.t0
        with self.lock:
            self.events.append((t, verb))
        print(f"  {t:6.3f} s  sent {verb:10s} {frame.hex(' ')}")

    def stop_recording(self) -> None:
        self.recording = False

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=1.0)
        self.ser.close()


def sleep_until(cap: Capture, t: float) -> None:
    remaining = t - (time.monotonic() - cap.t0)
    if remaining > 0:
        time.sleep(remaining)


def write_csv(cap: Capture) -> None:
    with CSV_PATH.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["t_s", "t_host_s", "vbus_mv", "duty", "flags", "event"])
        pending = sorted(cap.events)
        for t, t_host, vbus, duty, flags in cap.rows:
            label = ""
            while pending and pending[0][0] <= t:
                label = pending.pop(0)[1]
            w.writerow([f"{t:.4f}", f"{t_host:.4f}", vbus, duty, flags, label])


def write_svg(cap: Capture, plot_start: float, length: float) -> None:
    rows = cap.rows
    t = [r[0] for r in rows]
    vbus = [(r[2] / 1000.0) if r[2] is not None else None for r in rows]
    duty = [r[3] for r in rows]
    flags = [r[4] for r in rows]

    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(11, 7),
                             gridspec_kw={"height_ratios": [3, 3, 1]})
    window = f"{plot_start:g}-{length:g} s" if plot_start else f"{length:g} s"
    fig.suptitle(f"stream capture — {window}")

    axes[0].plot(t, vbus, lw=1.2, color="#1f77b4")
    axes[0].set_ylabel("vbus (V)")
    axes[1].plot(t, duty, lw=1.2, color="#d62728", drawstyle="steps-post")
    axes[1].set_ylabel("duty (/1000)")
    axes[2].plot(t, flags, lw=1.2, color="#7f7f7f", drawstyle="steps-post")
    axes[2].set_ylabel("flags")
    axes[2].set_xlabel("time (s)")

    colours = {"stop": "#333333"}
    for when, verb in cap.events:
        if not (plot_start <= when <= length):
            continue  # outside the plotted window; summarise() reports it
        colour = colours.get(verb, "#2ca02c")
        for ax in axes:
            ax.axvline(when, color=colour, ls="--", lw=1.0)
        axes[0].annotate(verb, xy=(when, 1.01), xycoords=("data", "axes fraction"),
                         ha="center", va="bottom", fontsize=8, color=colour)

    for ax in axes:
        ax.grid(alpha=0.3)
        ax.set_xlim(plot_start, length)

    fig.tight_layout()
    fig.savefig(SVG_PATH)
    plt.close(fig)


def summarise(cap: Capture, plot_start: float, length: float) -> None:
    rows = cap.rows
    print(f"\n{len(rows)} sets, {cap.parser.frames} packets, {cap.parser.resyncs} bytes resynced, {cap.partial} partial")
    if not rows:
        print("no telemetry received - is the board powered and streaming?")
        return
    vbus = [r[2] for r in rows if r[2] is not None]
    duty = [r[3] for r in rows if r[3] is not None]
    if vbus:
        print(f"  vbus  min {min(vbus)/1000:6.2f} V   max {max(vbus)/1000:6.2f} V   final {vbus[-1]/1000:6.2f} V")
    if duty:
        print(f"  duty  min {min(duty):4d}       max {max(duty):4d}       final {duty[-1]:4d}")
    # t_s is reconstructed from the set count, so cross-check it against the
    # wall clock. Drift means sets were lost, not merely delayed.
    if len(rows) > 1:
        board, host = rows[-1][0] - rows[0][0], rows[-1][1] - rows[0][1]
        if host > 0 and abs(board - host) > 0.05 * host:
            print(f"  WARNING board clock {board:.2f} s vs host {host:.2f} s"
                  f" - sets were dropped, t_s is not trustworthy")
    for when, verb in cap.events:
        if rows and when > rows[-1][0]:
            print(f"  note  {verb} sent at {when:.2f} s, after the capture window")
        elif not (plot_start <= when <= length):
            print(f"  note  {verb} at {when:.2f} s is outside the plotted window"
                  f" ({plot_start:g}-{length:g} s); it is still in the CSV")
    print(f"\nwrote {CSV_PATH.name} and {SVG_PATH.name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=sorted(MODES), default="cv", help="mode command to start")
    ap.add_argument("--start", type=float, default=2.0, help="seconds until the mode command")
    ap.add_argument("--end", type=float, default=5.0, help="seconds until STOP")
    ap.add_argument("--plot_start", type=float, default=0.0,
                    help="seconds at which the plot x-axis starts; capture still begins at 0")
    ap.add_argument("--length", type=float, default=8.0,
                    help="capture seconds; may be less than --end to stop after the plot")
    ap.add_argument("--port", help="serial port; auto-detected if omitted")
    args = ap.parse_args()

    if not (0 <= args.start < args.end and args.start < args.length):
        print("need 0 <= start < end and start < length", file=sys.stderr)
        return 2
    if not 0 <= args.plot_start < args.length:
        print("need 0 <= plot_start < length", file=sys.stderr)
        return 2

    port = args.port or console.pick_port()
    if port is None:
        print("no serial port found; try: uv run tools/console.py --list", file=sys.stderr)
        return 1

    try:
        cap = Capture(port)
    except serial.SerialException as exc:
        print(f"could not open {port}: {exc}", file=sys.stderr)
        return 1

    print(f"capturing {args.length:g} s on {port} at {console.BAUD}")
    try:
        sleep_until(cap, args.start)
        cap.send(args.mode)
        # --end may fall after --length, so run whichever comes first.
        for when, what in sorted([(args.end, "stop"), (args.length, "capture")]):
            sleep_until(cap, when)
            if what == "stop":
                cap.send("stop")
            else:
                cap.stop_recording()
    except KeyboardInterrupt:
        print("\ninterrupted - writing what was captured")
    finally:
        cap.close()

    write_csv(cap)  # always the full capture, regardless of --plot_start
    write_svg(cap, args.plot_start, args.length)
    summarise(cap, args.plot_start, args.length)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
