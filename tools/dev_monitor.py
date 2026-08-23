#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5", "matplotlib>=3.7"]
# ///
"""Print whatever the board sends over UART5, and optionally plot it.

The other half of Inc/dev/dev_reporter.h. That is a bench print-out, not a
protocol - this script shows you the lines as they arrive and does not decode
anything beyond "name=value".

Usage (uv resolves the dependencies from the metadata above - no venv needed):
    uv run tools/dev_monitor.py                # auto-detect the port
    uv run tools/dev_monitor.py -p COM7        # pick one explicitly
    uv run tools/dev_monitor.py --list         # show candidate ports
    uv run tools/dev_monitor.py --log run.txt  # also tee to a file
    uv run tools/dev_monitor.py --plot         # capture, write the SVG, exit

--plot never interrupts the stream: lines keep printing while it collects. It
is armed by the FIRST line carrying a name=value pair, so you can start before
the board has anything to say and it will wait - including through a
dev_record() run, which sends nothing at all until dev_flush().

The capture ends when the stream goes quiet, since dev_flush() sends its lines
back to back and any real pause means the burst is finished. Then it writes
serial_flush.svg in the repo root and exits, so there is no duration to guess
at. The optional TIMEOUT is only a safety net for a stream that never stops.
"""

import argparse
import re
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required. Run via 'uv run tools/dev_monitor.py', "
             "which installs it from the inline metadata above.")

BAUD = 115200

ROOT = Path(__file__).resolve().parent.parent
PLOT_PATH = ROOT / "serial_flush.svg"

# How long the stream has to stay silent before the capture is judged finished.
# dev_flush() transmits line after line with no gap, so any pause this long
# means the burst is over. Comfortably longer than the 1 s read timeout, so a
# single timed-out read cannot be mistaken for the end.
PLOT_IDLE_S = 1.5

# Matches the UART-to-USB bridges usually fitted to these boards. Only used to
# rank candidates for auto-detect; -p always wins.
LIKELY = ("cp210", "ch340", "ft232", "ftdi", "usb serial", "silicon labs")

# dev_reporter emits "[   12345] name=value name2=value2".
TICK_RE = re.compile(r"^\[\s*(\d+)\]")
PAIR_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+(?:\.\d+)?)")


def find_port():
    ports = list(list_ports.comports())
    if not ports:
        return None
    for p in ports:
        haystack = f"{p.description} {p.manufacturer or ''}".lower()
        if any(k in haystack for k in LIKELY):
            return p.device
    # Nothing recognisable - fall back to the only port, if there is only one.
    return ports[0].device if len(ports) == 1 else None


def parse(line):
    """-> (tick_ms or None, {name: float}). Empty dict if the line carries no
    name=value pairs, which is how a line is judged non-data."""
    tick = TICK_RE.match(line)
    return (int(tick.group(1)) if tick else None,
            {n: float(v) for n, v in PAIR_RE.findall(line)})


def save_plot(series, seconds):
    """Writes serial_flush.svg. One stacked subplot per series, sharing the
    time axis - separate axes rather than one, because vbus_mv runs to 25000
    while duty runs to 750, and on a shared scale the smaller series is a flat
    line along the bottom."""
    import matplotlib
    # Agg explicitly: this writes a file and never opens a window, so it must
    # not depend on a GUI toolkit existing in the uv-managed environment.
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if not series:
        print("# --plot: no name=value data captured, nothing to draw", flush=True)
        return

    fig, axes = plt.subplots(len(series), 1, sharex=True, squeeze=False,
                             figsize=(10, 2.2 * len(series)))
    for ax, (name, (t, v)) in zip(axes[:, 0], sorted(series.items())):
        ax.plot(t, v, linewidth=1.2, marker=".", markersize=3)
        ax.set_ylabel(name)
        ax.grid(True, alpha=0.3)

    axes[-1, 0].set_xlabel("board time (s)")
    fig.suptitle(f"dev_reporter - {seconds:g} s capture")
    fig.tight_layout()
    fig.savefig(PLOT_PATH, format="svg")
    plt.close(fig)

    points = sum(len(t) for t, _ in series.values())
    span = max((max(t) for t, _ in series.values() if t), default=0.0)
    print(f"# --plot: wrote {PLOT_PATH} - {points} points across "
          f"{len(series)} series, {span:.2f} s of board time", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("-b", "--baud", type=int, default=BAUD)
    ap.add_argument("--log", help="tee every line to this file as well")
    ap.add_argument("--plot", type=float, metavar="TIMEOUT", nargs="?", const=60.0,
                    help="capture until the stream goes idle, then write "
                         "serial_flush.svg and exit. TIMEOUT is only a safety "
                         "net, in seconds (default 60)")
    ap.add_argument("--list", action="store_true", help="list candidate ports and exit")
    args = ap.parse_args()

    if args.list:
        found = list(list_ports.comports())
        if not found:
            print("no serial ports found")
        for p in found:
            print(f"{p.device:12} {p.description}")
        return 0

    port = args.port or find_port()
    if port is None:
        sys.exit("could not pick a port automatically - use --list, then -p")

    log = open(args.log, "w", encoding="utf-8", buffering=1) if args.log else None

    # Capture state. `collecting` stays None until the first data line arrives,
    # so arming the script early costs nothing.
    series = {}          # name -> ([t_seconds], [value])
    collecting = None    # wall-clock monotonic time the first sample arrived
    last_data_at = None  # ... and the most recent one, for the idle test
    first_tick = None    # board tick of the first sample, for a zeroed x-axis
    want_plot = args.plot

    # The board transmits whether or not anyone is listening, so a reopened
    # terminal simply joins mid-stream. Nothing is buffered on its behalf.
    with serial.Serial(port, args.baud, timeout=1) as ser:
        print(f"# {port} @ {args.baud}  (ctrl-c to stop)", flush=True)
        if want_plot:
            print(f"# --plot armed: waiting for data, then {PLOT_IDLE_S:g} s of "
                  f"silence to end the capture ({want_plot:g} s timeout)", flush=True)
        try:
            while True:
                raw = ser.readline()

                if raw:
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    print(line, flush=True)
                    if log:
                        log.write(line + "\n")

                    if want_plot:
                        tick, values = parse(line)
                        if values:
                            last_data_at = time.monotonic()
                            if collecting is None:
                                collecting = last_data_at
                                first_tick = tick if tick is not None else 0
                            # Board tick for the x-axis - with a buffered flush
                            # it is the only record of when a sample was
                            # actually taken. Wall clock only decides when to
                            # stop collecting.
                            t = ((tick if tick is not None else first_tick)
                                 - first_tick) / 1000.0
                            for name, value in values.items():
                                series.setdefault(name, ([], []))
                                series[name][0].append(t)
                                series[name][1].append(value)

                # Tested on every pass, not only when a line arrived - after a
                # buffered dev_flush() the stream goes silent the instant the
                # burst ends, so a check that only ran on received lines would
                # never fire again.
                if want_plot and collecting is not None:
                    now = time.monotonic()
                    idle = now - last_data_at

                    if idle >= PLOT_IDLE_S:
                        reason = f"stream idle for {idle:.1f} s"
                    elif (now - collecting) >= want_plot:
                        # The safety net, not the normal path: a stream that
                        # never goes quiet would otherwise collect forever.
                        reason = f"timeout after {want_plot:g} s (capture may be cut short)"
                    else:
                        continue

                    print(f"# --plot: {reason}", flush=True)
                    save_plot(series, now - collecting)
                    return 0

        except KeyboardInterrupt:
            print()
        finally:
            if log:
                log.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
