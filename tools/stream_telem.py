#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""Stream MPPT telemetry from UART5 to the terminal.

The firmware emits one CSV line every 10 ms:

    <tick_ms>,<adc_raw>,<vbus_mv>

Usage (uv resolves pyserial from the inline metadata above - no venv needed):
    uv run tools/stream_telem.py                 # auto-detect the port
    uv run tools/stream_telem.py -p COM7         # pick one explicitly
    uv run tools/stream_telem.py --list          # show candidate ports
    uv run tools/stream_telem.py --csv out.csv   # also tee to a file
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


def parse(line):
    """Return (tick_ms, raw, vbus_mv) or None if the line is not telemetry."""
    parts = line.split(",")
    if len(parts) != 3:
        return None
    try:
        return int(parts[0]), int(parts[1]), int(parts[2])
    except ValueError:
        # Partial first line, or line noise on connect.
        return None


def main():
    ap = argparse.ArgumentParser(description="Stream MPPT telemetry over UART.")
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("-b", "--baud", type=int, default=BAUD, help=f"baud (default {BAUD})")
    ap.add_argument("--csv", help="also append raw lines to this file")
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
    print(f"{'uptime':>12}  {'raw':>6}  {'V_bus':>10}")

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

                tick_ms, adc_raw, vbus_mv = fields
                if sink:
                    sink.write(raw_line + "\n")

                print(f"{tick_ms / 1000:9.2f} s  {adc_raw:6d}  {vbus_mv / 1000:8.3f} V")
    except serial.SerialException as exc:
        sys.exit(f"\nSerial error: {exc}")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if sink:
            sink.close()


if __name__ == "__main__":
    main()
