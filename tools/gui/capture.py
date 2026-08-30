#!/usr/bin/env python3
"""Bench capture: assemble the stream into rows, write the CSV and the plots.

Shared by tools/plotter.py (headless CLI) and tools/gui/server.py (browser), so
the capture path has one implementation. The CSV column layout is part of the
contract with tools/iv_curve.py - changing it breaks replotting old files.

A Recorder is a sink, not a port owner: the caller feeds it decoded packets
from whatever SerialLink it already holds. Only one process can hold the COM
port, so a recorder has to be able to share the one that exists.
"""

from __future__ import annotations

import csv
import sys
import threading
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import console   # noqa: E402  - protocol definitions, single source
import iv_curve  # noqa: E402  - the I-V curve, drawn from the CSV

REPO_ROOT = Path(__file__).resolve().parents[2]
CAPTURE_DIR = REPO_ROOT / "captures"

# Row layout, in wire order. Indices into Recorder.rows.
T, T_HOST, VBUS_MV, VIN_MV, IIN_MA, DUTY, FLAGS = range(7)
CSV_HEADER = ["t_s", "t_host_s", "vbus_mv", "vin_mv", "iin_ma", "duty", "flags", "event"]


class Recorder:
    """Accumulates completed telemetry sets into rows.

    A set is bracketed by console.STREAM_FIRST and STREAM_LAST. A set that did
    not deliver every packet is counted but still written, matching what the
    CSV has always carried; iv_curve.py drops those rows on the flags column.
    """

    def __init__(self) -> None:
        self.rows: list[tuple] = []
        self.events: list[tuple[float, str]] = []
        self.latest: dict[str, int] = {}
        self.ticks = -1   # sets seen; the board emits one every STREAM_PERIOD_MS
        self.partial = 0  # sets that did not deliver every packet
        self.complete = True
        self.recording = True
        self.lock = threading.Lock()

    def feed(self, name: str, value: int, t_host: float) -> None:
        if not self.recording:
            return
        with self.lock:
            if name == console.STREAM_FIRST:
                if self.ticks >= 0 and not self.complete:
                    self.partial += 1
                self.ticks += 1
                self.complete = False
            self.latest[name] = value
            if name == console.STREAM_LAST:
                self.complete = True
                self.rows.append((
                    self.ticks * console.STREAM_PERIOD_MS / 1000.0,
                    t_host,
                    self.latest.get("vbus_mv"),
                    self.latest.get("vin_mv"),
                    self.latest.get("iin_ma"),
                    self.latest.get("duty"),
                    self.latest.get("flags"),
                ))

    def mark(self, t_host: float, label: str) -> None:
        with self.lock:
            self.events.append((t_host, label))

    def stop(self) -> None:
        self.recording = False

    def write_csv(self, path: Path) -> None:
        with self.lock:
            rows, events = list(self.rows), sorted(self.events)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(CSV_HEADER)
            pending = list(events)
            for row in rows:
                label = ""
                while pending and pending[0][0] <= row[T]:
                    label = pending.pop(0)[1]
                w.writerow([f"{row[T]:.4f}", f"{row[T_HOST]:.4f}", *row[VBUS_MV:], label])


# --------------------------------------------------------------------------
# Sequences: a scripted bench run, defined as data.
# --------------------------------------------------------------------------
@dataclass(frozen=True)
class Sequence:
    """One scripted bench run. Adding a routine is a table entry, not code.

    steps are (seconds from the start of the run, verb); verbs are
    console.OPCODES keys. renders names the plots written when it finishes.
    """
    name: str
    label: str
    steps: tuple[tuple[float, str], ...]
    length: float
    renders: tuple[str, ...] = ("timeseries",)
    plot_start: float = 0.0
    description: str = ""

    def validate(self) -> None:
        for when, verb in self.steps:
            if verb not in console.OPCODES:
                raise ValueError(f"{self.name}: {verb} is not in console.OPCODES")
            if when < 0:
                raise ValueError(f"{self.name}: step at {when} s is before the start")
        if self.length <= 0 or not 0 <= self.plot_start < self.length:
            raise ValueError(f"{self.name}: need 0 <= plot_start < length")

    def as_json(self) -> dict:
        return {"name": self.name, "label": self.label, "length": self.length,
                "steps": [[w, v] for w, v in self.steps],
                "renders": list(self.renders), "description": self.description}


SEQUENCES: tuple[Sequence, ...] = (
    Sequence(
        name="cv",
        label="CV step",
        steps=((2.0, "cv"), (5.0, "stop")),
        length=8.0,
        description="Constant-voltage loop for 3 s, with idle either side.",
    ),
    Sequence(
        name="ivsweep",
        label="I-V sweep",
        steps=((2.0, "ivsweep"), (30.0, "stop")),
        length=32.0,
        renders=("timeseries", "iv"),
        description="Full duty sweep; also renders the I-V curve, one line per pass.",
    ),
    Sequence(
        name="mppt",
        label="MPPT run",
        steps=((2.0, "mppt"), (30.0, "stop")),
        length=32.0,
        renders=("timeseries", "iv"),
        description="Perturb-and-observe tracking for 28 s.",
    ),
    Sequence(
        name="chmppt",
        label="Single-channel MPPT",
        steps=((2.0, "chmppt"), (30.0, "stop")),
        length=32.0,
        renders=("timeseries", "iv"),
        description="Channel-A MPPT for 28 s.",
    ),
)

SEQUENCES_BY_NAME = {s.name: s for s in SEQUENCES}
for _seq in SEQUENCES:
    _seq.validate()


def capture_paths(name: str, when: datetime | None = None) -> dict[str, Path]:
    """Timestamped output set, so one run never overwrites the last."""
    stamp = (when or datetime.now()).strftime("%Y%m%d-%H%M%S")
    stem = CAPTURE_DIR / f"{stamp}_{name}"
    return {"csv": Path(f"{stem}.csv"),
            "timeseries": Path(f"{stem}.svg"),
            "iv": Path(f"{stem}_iv.svg")}


class SequenceRun:
    """Drives a Sequence's steps against a send callable, on its own thread.

    Sleeps on an Event rather than time.sleep so cancel() lands promptly: a
    32 s sweep that cannot be stopped early keeps the board switching for all
    of it, which is the one thing a bench operator needs to be able to undo.
    """

    def __init__(self, seq: Sequence, send, recorder: Recorder, clock,
                 on_event=None) -> None:
        self.seq = seq
        self.send = send
        self.recorder = recorder
        self.clock = clock          # () -> seconds since the link opened
        self.on_event = on_event or (lambda *a: None)
        self.t0 = clock()
        self.cancelled = threading.Event()
        self.finished = threading.Event()
        self.error: str | None = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> "SequenceRun":
        self.thread.start()
        return self

    @property
    def elapsed(self) -> float:
        return self.clock() - self.t0

    def _wait_until(self, offset: float) -> bool:
        """True if the wait completed, False if it was cancelled."""
        remaining = offset - self.elapsed
        if remaining > 0:
            return not self.cancelled.wait(remaining)
        return not self.cancelled.is_set()

    def _run(self) -> None:
        stopped = False
        try:
            # length ends the *recording*, not the run: a step may fall after
            # it, and cutting the run short there would skip the stop command.
            timeline = sorted([*self.seq.steps, (self.seq.length, None)],
                              key=lambda step: step[0])
            for when, verb in timeline:
                if not self._wait_until(when):
                    break
                if verb is None:
                    self.recorder.stop()
                    continue
                self.send(verb)
                self.recorder.mark(self.clock(), verb)
                stopped = stopped or verb == "stop"
                self.on_event("step", verb)
        except Exception as exc:                 # noqa: BLE001 - surfaced in the UI
            self.error = str(exc)
        finally:
            self.recorder.stop()
            # Cancelling mid-run leaves the board switching. Stopping it is the
            # whole point of the cancel, so it is sent even on the error path.
            if not stopped:
                try:
                    self.send("stop")
                except Exception:                # noqa: BLE001
                    pass
            self.finished.set()
            self.on_event("finished", None)

    def cancel(self) -> None:
        self.cancelled.set()

    def join(self, timeout: float | None = None) -> None:
        self.thread.join(timeout)


def render(seq: Sequence, recorder: Recorder, paths: dict[str, Path],
           rload: float | None = None) -> tuple[list[str], dict[str, Path]]:
    """Write every output the sequence asks for. Returns (summary, files written)."""
    recorder.write_csv(paths["csv"])
    written = {"csv": paths["csv"]}
    lines = summarise(recorder)

    if "timeseries" in seq.renders:
        render_timeseries(recorder, paths["timeseries"], seq)
        written["timeseries"] = paths["timeseries"]
    if "iv" in seq.renders:
        result = iv_curve.render(paths["csv"], paths["iv"], rload, True,
                                 f"channel A input V-I - {paths['csv'].name}")
        if result is not None:
            written["iv"] = paths["iv"]
            duty, volts, amps, pin = result
            lines.append(f"MPP {volts:.3f} V  {amps:.3f} A  {pin:.2f} W  at duty {duty}")
    return lines, written


def summarise(recorder: Recorder) -> list[str]:
    rows = recorder.rows
    lines = [f"{len(rows)} sets, {recorder.partial} partial"]
    if not rows:
        lines.append("no telemetry received - is the board powered and streaming?")
        return lines

    def span(index: int, divisor: float, unit: str, name: str, fmt: str = "6.2f") -> None:
        vals = [r[index] for r in rows if r[index] is not None]
        if not vals:
            return
        lo, hi, last = min(vals) / divisor, max(vals) / divisor, vals[-1] / divisor
        lines.append(f"{name:4s} min {lo:{fmt}} {unit}  max {hi:{fmt}} {unit}"
                     f"  final {last:{fmt}} {unit}")

    span(VIN_MV, 1000.0, "V", "vin", "6.3f")
    span(IIN_MA, 1000.0, "A", "iin", "6.3f")
    span(VBUS_MV, 1000.0, "V", "vbus")
    span(DUTY, 1.0, " ", "duty", "6.0f")

    # t_s is reconstructed from the set count, so cross-check it against the
    # wall clock. Drift means sets were lost, not merely delayed.
    if len(rows) > 1:
        board, host = rows[-1][T] - rows[0][T], rows[-1][T_HOST] - rows[0][T_HOST]
        if host > 0 and abs(board - host) > 0.05 * host:
            lines.append(f"WARNING board clock {board:.2f} s vs host {host:.2f} s"
                         f" - sets were dropped, t_s is not trustworthy")
    return lines


def render_timeseries(recorder: Recorder, path: Path, seq: Sequence) -> None:
    # Imported here, not at module scope: the GUI does not need matplotlib
    # until a sequence finishes, and importing it costs about a second.
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rows = recorder.rows
    if not rows:
        return
    plot_start, length = seq.plot_start, seq.length
    t = [r[T] for r in rows]

    def scaled(index: int, divisor: float) -> list:
        return [(r[index] / divisor) if r[index] is not None else None for r in rows]

    fig, axes = plt.subplots(4, 1, sharex=True, figsize=(11, 9),
                             gridspec_kw={"height_ratios": [3, 3, 3, 1]})
    window = f"{plot_start:g}-{length:g} s" if plot_start else f"{length:g} s"
    fig.suptitle(f"{seq.label} - {window}")

    axes[0].plot(t, scaled(VIN_MV, 1000.0), lw=1.2, color="#ff7f0e", label="vin (ch A)")
    axes[0].plot(t, scaled(VBUS_MV, 1000.0), lw=1.2, color="#1f77b4", label="vbus")
    axes[0].set_ylabel("volts")
    axes[0].legend(loc="upper left", fontsize=8)
    axes[1].plot(t, scaled(IIN_MA, 1000.0), lw=1.2, color="#9467bd")
    axes[1].set_ylabel("iin (A)")
    axes[1].axhline(0.0, color="#999999", lw=0.8)
    axes[2].plot(t, [r[DUTY] for r in rows], lw=1.2, color="#d62728",
                 drawstyle="steps-post")
    axes[2].set_ylabel("duty (/1000)")
    axes[3].plot(t, [r[FLAGS] for r in rows], lw=1.2, color="#7f7f7f",
                 drawstyle="steps-post")
    axes[3].set_ylabel("flags")
    axes[3].set_xlabel("time (s)")

    colours = {"stop": "#333333"}
    for when, verb in recorder.events:
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
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path)
    plt.close(fig)


def list_captures(limit: int = 40) -> list[dict]:
    """Past runs, newest first. The GUI's history list."""
    if not CAPTURE_DIR.is_dir():
        return []
    out = []
    for csv_path in sorted(CAPTURE_DIR.glob("*.csv"), reverse=True)[:limit]:
        if csv_path.stem.endswith("_iv"):
            continue
        stem = csv_path.with_suffix("")
        files = {"csv": csv_path.name}
        for key, candidate in (("timeseries", Path(f"{stem}.svg")),
                               ("iv", Path(f"{stem}_iv.svg"))):
            if candidate.exists():
                files[key] = candidate.name
        out.append({"name": csv_path.stem, "files": files,
                    "size": csv_path.stat().st_size,
                    "mtime": csv_path.stat().st_mtime})
    return out
