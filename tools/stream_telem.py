#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""Stream MPPT telemetry from UART5 to the terminal.

For plots and series selection use tools/telem_gui.py instead; this is the
quick sanity check.

The firmware emits one CSV line every 50 ms (23 integer fields):

    <tick_ms>,<valid_mask>,<vbus_mv>,
    <ch1_vin_mv>,<ch1_iin_ma>,<ch1_vout_mv>,<ch1_iout_ma>,  ... through ch5

The firmware also emits PWM config reports and command replies, which start
with '#'. They are skipped here (wrong field count) - telem_gui.py is the tool
that reads them, and the one that can send commands back.

Usage (uv resolves pyserial from the inline metadata above - no venv needed):
    uv run tools/stream_telem.py                 # auto-detect the port
    uv run tools/stream_telem.py -p COM7         # pick one explicitly
    uv run tools/stream_telem.py --all           # every channel, not just V_BUS
    uv run tools/stream_telem.py --list          # show candidate ports
    uv run tools/stream_telem.py --csv out.csv   # also tee raw lines to a file
"""

import argparse
import sys

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required. Run via 'uv run tools/stream_telem.py', "
             "which installs it from the inline metadata above.")

BAUD = 115200

# Matches the UART-to-USB bridges usually fitted to these boards. Only used
# to rank candidates for auto-detect; -p always wins.
LIKELY = ("cp210", "ch340", "ft232", "ftdi", "usb serial", "silicon labs")


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


def show_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"  {p.device:10s}  {p.description}")


FIELD_COUNT = 23


def parse(line):
    """Return (tick_ms, valid_mask, vbus_mv, [ch1..ch5 quads]) or None."""
    parts = line.split(",")
    if len(parts) != FIELD_COUNT:
        return None
    try:
        raw = [int(p) for p in parts]
    except ValueError:
        # Partial first line, or line noise on connect.
        return None
    channels = [raw[3 + 4 * c: 7 + 4 * c] for c in range(5)]
    return raw[0], raw[1], raw[2], channels


def main():
    ap = argparse.ArgumentParser(description="Stream MPPT telemetry over UART.")
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("-b", "--baud", type=int, default=BAUD, help=f"baud (default {BAUD})")
    ap.add_argument("--csv", help="also append raw lines to this file")
    ap.add_argument("--all", action="store_true",
                    help="show all five channels, not just V_BUS")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    args = ap.parse_args()

    if args.list:
        show_ports()
        return

    port = args.port or find_port()
    if not port:
        print("Could not auto-detect a port. Use -p, or --list to see options.\n")
        show_ports()
        sys.exit(1)

    print(f"Listening on {port} @ {args.baud} ... Ctrl+C to stop.\n")
    if args.all:
        header = f"{'uptime':>10}  {'V_BUS':>9}  " + "  ".join(
            f"{'CH' + str(c) + ' Vin':>8} {'Iin':>8} {'Vout':>8} {'Iout':>8}"
            for c in range(1, 6))
    else:
        header = f"{'uptime':>10}  {'V_BUS':>9}  {'channels ok':>11}"
    print(header)

    sink = open(args.csv, "a", encoding="utf-8") if args.csv else None

    try:
        # timeout lets KeyboardInterrupt land promptly on a silent link.
        with serial.Serial(port, args.baud, timeout=1) as ser:
            while True:
                raw_line = ser.readline().decode("ascii", errors="replace").strip()
                if not raw_line:
                    continue

                fields = parse(raw_line)
                if fields is None:
                    continue

                tick_ms, valid_mask, vbus_mv, channels = fields
                if sink:
                    sink.write(raw_line + "\n")

                row = f"{tick_ms / 1000:8.2f} s  {vbus_mv / 1000:7.3f} V"
                if args.all:
                    for vin, iin, vout, iout in channels:
                        row += (f"  {vin / 1000:8.3f} {iin / 1000:8.3f}"
                                f" {vout / 1000:8.3f} {iout / 1000:8.3f}")
                else:
                    ok = "".join(str(c + 1) if valid_mask & (1 << c) else "-"
                                 for c in range(5))
                    row += f"  {ok:>11}"
                print(row)
    except serial.SerialException as exc:
        sys.exit(f"\nSerial error: {exc}")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if sink:
            sink.close()


if __name__ == "__main__":
    main()
