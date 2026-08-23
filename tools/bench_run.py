#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Build, flash, and capture - the Ctrl+Shift+B sequence plus a plot.

Does what the "Build & Flash" task does (CMake build, then STM32CubeProgrammer
with -rst so the board starts running), then hands straight over to
dev_monitor.py with --plot. The board is already running by the time the
monitor opens, and --plot arms on the first name=value line rather than on a
timer, so there is no race to lose.

    uv run tools/bench_run.py              # build, flash, capture, write the SVG
    uv run tools/bench_run.py -t 120       # raise the safety-net timeout
    uv run tools/bench_run.py -p COM7      # name the serial port
    uv run tools/bench_run.py --log run.txt
    uv run tools/bench_run.py --monitor-only   # skip build and flash
    uv run tools/bench_run.py --no-flash       # build, but do not flash

This has no dependencies of its own; dev_monitor.py is launched through
`uv run` so it resolves pyserial and matplotlib from its own metadata.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "build" / "Debug" / "gan_mppt_firmware.elf"
MONITOR = Path(__file__).resolve().parent / "dev_monitor.py"

# CubeCLT's bin directories are on the machine PATH (see .agents/workflow.md),
# so the bare name normally resolves. The absolute path is the same one
# .vscode/tasks.json uses, kept as a fallback for a shell without it.
PROGRAMMER_FALLBACK = Path(
    r"C:/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe")


def run(step, cmd):
    print(f"\n=== {step} ===\n$ {subprocess.list2cmdline(cmd)}", flush=True)
    result = subprocess.run(cmd, cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f"\n{step} failed (exit {result.returncode}) - stopping here")


def programmer():
    found = shutil.which("STM32_Programmer_CLI")
    if found:
        return found
    if PROGRAMMER_FALLBACK.is_file():
        return str(PROGRAMMER_FALLBACK)
    sys.exit("STM32_Programmer_CLI not on PATH and not at the CubeCLT default - "
             "install STM32CubeCLT or flash from VS Code instead")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-t", "--time", type=float, default=60.0, metavar="TIMEOUT",
                    help="safety-net timeout for the capture, in seconds "
                         "(default: 60). The capture normally ends when the "
                         "flush burst stops arriving, not on this.")
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("--log", help="tee the captured lines to this file")
    ap.add_argument("--monitor-only", action="store_true",
                    help="skip build and flash, just capture")
    ap.add_argument("--no-flash", action="store_true", help="build but do not flash")
    args = ap.parse_args()

    uv = shutil.which("uv")
    if uv is None:
        sys.exit("uv is not on PATH - this repo runs its Python tools with uv")

    if not args.monitor_only:
        # The same preset CMake Tools builds, so this and Ctrl+Shift+B produce
        # the same binary rather than two different build trees.
        run("build", ["cmake", "--build", "--preset", "Debug"])

        if not args.no_flash:
            if not ELF.is_file():
                sys.exit(f"no ELF at {ELF} - did the build actually produce one?")
            # -rst so the board runs straight away; no debug session is started.
            run("flash", [programmer(), "-c", "port=SWD", "-w", str(ELF), "-v", "-rst"])

    monitor = [uv, "run", str(MONITOR), "--plot", str(args.time)]
    if args.port:
        monitor += ["-p", args.port]
    if args.log:
        monitor += ["--log", args.log]

    print(f"\n=== capture (ends when the flush burst stops; {args.time:g} s timeout) ===\n"
          f"$ {subprocess.list2cmdline(monitor)}", flush=True)

    # Handed over rather than wrapped: ctrl-c and the plot window belong to
    # dev_monitor, and this script has nothing left to do.
    return subprocess.run(monitor, cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())
