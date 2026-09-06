#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.8"]
# ///
"""Draw an I-V curve from a stream_plot.csv capture."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CSV = REPO_ROOT / "stream_plot.csv"
DEFAULT_SVG = REPO_ROOT / "stream_iv.svg"

REQUIRED = ("vin_mv", "iin_ma", "duty")

UP_C, DOWN_C, IIN_C, PIN_C = "#ff7f0e", "#1f77b4", "#9467bd", "#2ca02c"


def read_rows(path: Path) -> list[dict]:
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        missing = [c for c in REQUIRED if c not in (reader.fieldnames or ())]
        if missing:
            raise SystemExit(f"{path.name} has no {', '.join(missing)} column - "
                             "it predates the vin/iin packets in Src/app/stream.c")
        rows = []
        for raw in reader:
            row = {k: (int(v) if v not in ("", None) else None)
                   for k, v in raw.items() if k not in ("t_s", "t_host_s", "event")}
            row["t_s"] = float(raw["t_s"])
            row["event"] = raw.get("event", "")
            rows.append(row)
    return rows


def trim_to_sweep(rows: list[dict]) -> list[dict]:
    start, end = 0, len(rows)
    for n, row in enumerate(rows):
        event = row["event"]
        if event and event != "stop" and start == 0:
            start = n
        elif event == "stop":
            end = n
            break
    return rows[start:end]


def settled_samples(rows: list[dict]) -> list[tuple[int, float, float, float]]:
    running_known = any((row.get("flags") or 0) & 0x02 for row in rows)

    out: list[tuple[int, float, float, float]] = []
    cur_duty: int | None = None
    settled: tuple[int, float, float, float] | None = None
    previous: tuple[int, int] | None = None
    for row in rows:
        if row["duty"] is None or row["vin_mv"] is None or row["iin_ma"] is None:
            continue
        if row.get("flags") is not None and not (row["flags"] & 0x01):
            continue
        if running_known and not (row["flags"] & 0x02):
            continue
        if row["duty"] != cur_duty:
            if settled is not None:
                out.append(settled)
            cur_duty, settled = row["duty"], None
        sample = (row["vin_mv"], row["iin_ma"])
        if sample != previous:
            previous = sample
            settled = (cur_duty, sample[0] / 1000.0, sample[1] / 1000.0,
                       (row["vbus_mv"] or 0) / 1000.0)
    if settled is not None:
        out.append(settled)
    return out


def split_passes(samples: list[tuple]) -> list[list[tuple]]:
    if len(samples) < 2:
        return [samples] if samples else []
    passes: list[list[tuple]] = []
    current = [samples[0]]
    direction = 0
    for prev, item in zip(samples, samples[1:]):
        step = item[0] - prev[0]
        if step == 0:
            current.append(item)
            continue
        heading = 1 if step > 0 else -1
        if direction and heading != direction:
            passes.append(current)
            current = [prev, item]
        else:
            current.append(item)
        direction = heading
    passes.append(current)
    return passes


def ascending(one_pass: list[tuple]) -> bool:
    return one_pass[-1][0] >= one_pass[0][0]


def pass_style(index: int, total: int, up: bool) -> dict:
    return {"color": UP_C if up else DOWN_C,
            "alpha": 0.35 + 0.65 * (index / max(total - 1, 1))}


def direction_legend():
    return [plt.Line2D([], [], color=UP_C, label="0 -> MAX"),
            plt.Line2D([], [], color=DOWN_C, label="MAX -> 0")]


def render(csv_path: Path, svg_path: Path, rload: float | None = None,
           trim: bool = True, title: str | None = None) -> tuple | None:

    rows = read_rows(csv_path)
    if trim:
        rows = trim_to_sweep(rows)
    samples = settled_samples(rows)
    if len(samples) < 2:
        print(f"  only {len(samples)} settled duty step(s) in {csv_path.name};"
              f" skipping {svg_path.name}")
        return None
    passes = split_passes(samples)
    many = len(passes) > 1

    duty = [s[0] for s in samples]
    v = [s[1] for s in samples]
    i = [s[2] for s in samples]
    pin = [a * b for a, b in zip(v, i)]
    mpp = max(range(len(samples)), key=lambda k: pin[k])

    fig, (ax_iv, ax_d) = plt.subplots(1, 2, figsize=(12, 5))
    shape = (f"{len(passes)} passes / {len(passes) / 2:g} cycles" if many
             else f"{len(samples)} duty steps")
    fig.suptitle(title or f"channel A input V-I - {csv_path.name}, {shape}")


    for n, one in enumerate(passes):
        ax_iv.plot([q[2] for q in one], [q[1] for q in one], "-o", ms=2.5, lw=1.0,
                   **pass_style(n, len(passes), ascending(one)))

    ax_iv.set_xlabel("iin (A)")
    ax_iv.set_ylabel("vin (V)")
    ax_iv.set_xlim(left=0)
    ax_iv.set_ylim(bottom=0)
    ax_iv.grid(alpha=0.3)


    ax_iv.plot(i[mpp], v[mpp], "*", ms=16, color=PIN_C, zorder=5)
    ax_iv.annotate(f"{v[mpp]:.2f} V, {i[mpp]:.2f} A, {pin[mpp]:.1f} W @ D={duty[mpp]}",
                   xy=(i[mpp], v[mpp]), xytext=(-10, -14), textcoords="offset points",
                   fontsize=8, color=PIN_C, ha="right", va="top")
    if many:
        ax_iv.legend(handles=direction_legend(), loc="lower left", fontsize=8)

    if many:


        for n, one in enumerate(passes):
            ax_d.plot([q[0] for q in one], [q[1] * q[2] for q in one], "-", lw=1.0,
                      **pass_style(n, len(passes), ascending(one)))
        ax_d.set_ylabel("pin (W)")
        ax_d.legend(handles=direction_legend(), loc="upper left", fontsize=8)
    else:


        ax_d.plot(duty, v, "-o", ms=3, lw=1.0, color=UP_C, label="vin (V)")
        ax_d.plot(duty, i, "-o", ms=3, lw=1.0, color=IIN_C, label="iin (A)")
        ax_d.plot(duty, pin, "-o", ms=3, lw=1.0, color=PIN_C, label="pin (W)")
        ax_d.legend(loc="upper left", fontsize=8)
    ax_d.axvline(duty[mpp], color=PIN_C, ls="--", lw=0.8)
    ax_d.set_xlabel("duty (/1000)")
    ax_d.grid(alpha=0.3)

    if rload:

        eff = [(s[0], 100.0 * (s[3] ** 2 / rload) / (s[1] * s[2]))
               for s in samples if s[1] * s[2] > 0.5]
        if eff:
            peak = max(e[1] for e in eff)
            if peak > 100.0:


                print(f"  --rload {rload:g} gives {peak:.0f} % efficiency, which is"
                      f" impossible - vbus is not across a {rload:g} ohm load."
                      f" Dropping the efficiency trace.")
            else:
                ax_e = ax_d.twinx()
                ax_e.plot([e[0] for e in eff], [e[1] for e in eff], ".", ms=3,
                          color=IIN_C)
                ax_e.set_ylabel(f"efficiency (%) into {rload:g} ohm", color=IIN_C)
                ax_e.set_ylim(0, 105)
                print(f"  efficiency  min {min(e[1] for e in eff):5.1f} %"
                      f"   max {peak:5.1f} %")

    fig.tight_layout()
    fig.savefig(svg_path)
    plt.close(fig)

    if many:
        best = [max(one, key=lambda q: q[1] * q[2])[0] for one in passes]
        print(f"  {len(passes)} passes, peak-power duty {min(best)}-{max(best)}"
              f" (spread {max(best) - min(best)})")
    print(f"  MPP {v[mpp]:.3f} V  {i[mpp]:.3f} A  {pin[mpp]:.2f} W  at duty {duty[mpp]}")
    return duty[mpp], v[mpp], i[mpp], pin[mpp]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?", type=Path, default=DEFAULT_CSV,
                    help=f"capture to read (default {DEFAULT_CSV.name})")
    ap.add_argument("--out", type=Path, default=DEFAULT_SVG,
                    help=f"where to write the curve (default {DEFAULT_SVG.name})")
    ap.add_argument("--rload", type=float,
                    help="output load in ohms; adds stage efficiency from vbus")
    ap.add_argument("--all", action="store_true",
                    help="plot every row, not just between the mode command and STOP")
    ap.add_argument("--title", help="override the figure title")
    args = ap.parse_args()

    if not args.csv.exists():
        print(f"{args.csv} not found - run tools/plotter.py first", file=sys.stderr)
        return 1

    if render(args.csv, args.out, args.rload, not args.all, args.title) is None:
        return 1
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
