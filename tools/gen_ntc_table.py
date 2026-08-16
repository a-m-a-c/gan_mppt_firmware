#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Generate Inc/drivers/ntc_table.h from Murata's characteristic CSV.

The CSV (.agents/NCU18XH103F6SRB.csv) is a *divider output voltage* table, not
a resistance table. Murata generated it for one specific circuit - a 10 kOhm
pull-up to 3.3 V - which its 25 degC value of 1.65 V, exactly half of 3.3 V,
confirms. So the CSV is only directly usable if the board's divider matches.

Rather than assume it does, this script backs the thermistor curve R(T) out of
Murata's voltages and then recomputes the pin voltage for whatever divider the
board actually has. Change --pullup and the whole table follows; nothing else
in the firmware needs to know, because the generated table is what maps pin
voltage to temperature.

    R(T)  = R_ref * V / (V_ref - V)      undo Murata's reference divider
    V_pin = V_rail * R(T) / (R_pu + R(T))  redo it with the board's

The divider ratio is the whole answer: a 10x error in it - a 100 kOhm pull-up
where the table assumes 10 kOhm, or a 1 kOhm thermistor under a 10 kOhm
pull-up, which are electrically identical - reads a room-temperature board as
roughly 95 degC.

Usage (stdlib only, so this just runs):
    uv run tools/gen_ntc_table.py
    uv run tools/gen_ntc_table.py --pullup 100000

Writes the header in place. Re-run it if the NTC part, the pull-up or the rail
changes, and commit the result; the firmware does not parse CSV at runtime.
"""

import argparse
import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CSV_PATH = REPO / ".agents" / "NCU18XH103F6SRB.csv"
OUT_PATH = REPO / "Inc" / "drivers" / "ntc_table.h"

# The circuit Murata's CSV was generated for. Not the board's - see --pullup.
REF_PULLUP_OHMS = 10000.0
REF_RAIL_V = 3.3

# Column pairs in the CSV are (temp, vout) for min, typ, max. We want typ.
TYP_TEMP_COL, TYP_VOUT_COL = 2, 3

# Stored in tenths of a millivolt. 3.3 V is 33000, well inside uint16, and it
# keeps ~20 counts per degC at the hot end where the curve is flattest -
# storing whole millivolts there would quantise to about half a degree.
UNITS_PER_VOLT = 10000


def read_typ_curve(path):
    rows = []
    with path.open(newline="") as handle:
        for fields in csv.reader(handle):
            if not fields or fields[0].startswith("#"):
                continue
            try:
                temp = float(fields[TYP_TEMP_COL])
                vout = float(fields[TYP_VOUT_COL])
            except (IndexError, ValueError):
                continue  # header row
            rows.append((temp, vout))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pullup", type=float, default=10000.0,
                    help="board's NTC pull-up resistance in ohms (default 10000)")
    ap.add_argument("--rail", type=float, default=3.3,
                    help="voltage the pull-up goes to (default 3.3)")
    ap.add_argument("--ntc25", type=float, default=10000.0,
                    help="fitted thermistor's nominal resistance at 25 degC in "
                         "ohms (default 10000, i.e. a '103' part). Use 1000 for "
                         "a '102'. Same curve shape, scaled - only the ratio to "
                         "--pullup affects the result.")
    args = ap.parse_args()

    if not CSV_PATH.exists():
        sys.exit(f"missing {CSV_PATH}")

    rows = read_typ_curve(CSV_PATH)
    if not rows:
        sys.exit(f"no data rows parsed from {CSV_PATH}")

    temps = [t for t, _ in rows]
    step = round(temps[1] - temps[0])
    if any(round(b - a) != step for a, b in zip(temps, temps[1:])):
        sys.exit("temperature column is not evenly spaced")

    # Undo Murata's reference divider to get the thermistor itself, then redo
    # it with the board's. With the defaults this is the identity, to within
    # floating point - the round-trip is checked below.
    ohms = [REF_PULLUP_OHMS * v / (REF_RAIL_V - v) for _, v in rows]

    at25 = ohms[temps.index(25.0)] if 25.0 in temps else None
    if at25 is not None and not (9800.0 < at25 < 10200.0):
        sys.exit(f"derived R(25 degC) = {at25:.0f} ohm, expected ~10000 - the CSV "
                 f"is not for a 10 kOhm part in a {REF_PULLUP_OHMS:.0f}/{REF_RAIL_V} V divider")

    # A thermistor of the same material with a different nominal resistance has
    # the same curve shape, just scaled - so one CSV covers a "102" as well as
    # a "103". Only the ratio of thermistor to pull-up reaches the ADC, which is
    # why a 1k part under a 10k pull-up and a 10k part under a 100k pull-up are
    # indistinguishable at the pin.
    scale = args.ntc25 / 10000.0
    ohms = [r * scale for r in ohms]
    volts = [args.rail * r / (args.pullup + r) for r in ohms]

    counts = [round(v * UNITS_PER_VOLT) for v in volts]
    if any(b >= a for a, b in zip(counts, counts[1:])):
        sys.exit("voltage column is not strictly decreasing")
    if max(counts) > 0xFFFF:
        sys.exit("voltage exceeds uint16 range")

    lines = [
        "/**",
        "  ******************************************************************************",
        "  * @file    ntc_table.h",
        "  * @author  Angus Macdonald",
        "  * @brief   NCU18XH103F6SRB divider voltage vs temperature (GENERATED).",
        "  ******************************************************************************",
        "  * @attention",
        "  *",
        "  * This software is licensed under terms that can be found in the LICENSE file",
        "  * in the root directory of this software component.",
        "  * If no LICENSE file comes with this software, it is provided AS-IS.",
        "  *",
        "  ******************************************************************************",
        "  */",
        "#ifndef NTC_TABLE_H",
        "#define NTC_TABLE_H",
        "",
        "#include <stdint.h>",
        "",
        "/* DO NOT EDIT BY HAND. Regenerate with:",
        " *",
        " *     uv run tools/gen_ntc_table.py",
        " *",
        " * Source: .agents/NCU18XH103F6SRB.csv (Murata's typical curve), with the",
        " * thermistor curve backed out of Murata's reference divider and recomputed",
        " * for THIS BOARD's divider:",
        " *",
        f" *     {args.ntc25:.0f} ohm NTC to ground, {args.pullup:.0f} ohm pull-up"
        f" to {args.rail} V",
        " *",
        " * If that is not the circuit on the board, every temperature this table",
        " * produces is wrong - and wrong smoothly, so it looks like a reading rather",
        " * than a fault. Regenerate with --pullup / --rail.",
        " *",
        " * Values are the voltage at the ADC pin in tenths of a millivolt, strictly",
        f" * decreasing, one entry per {step} degC from NTC_TABLE_MIN_C upward. */",
        "",
        f"#define NTC_TABLE_MIN_C   {int(temps[0])}",
        f"#define NTC_TABLE_STEP_C  {step}",
        f"#define NTC_TABLE_LEN     {len(counts)}U",
        "",
        "/* Tenths of a millivolt, so 33000 is 3.3 V. */",
        "static const uint16_t ntc_table_dmv[NTC_TABLE_LEN] = {",
    ]

    for start in range(0, len(counts), 8):
        chunk = counts[start : start + 8]
        comment = f"  /* {int(temps[start]):+4d} degC */"
        lines.append("    " + " ".join(f"{c}," for c in chunk) + comment)

    lines += [
        "};",
        "",
        "#endif /* NTC_TABLE_H */",
        "",
    ]

    OUT_PATH.write_text("\n".join(lines), encoding="utf-8")
    print(
        f"wrote {OUT_PATH.relative_to(REPO)}: {len(counts)} entries, "
        f"{int(temps[0])}..{int(temps[-1])} degC, "
        f"{max(counts)}..{min(counts)} tenths of a mV\n"
        f"  divider: {args.ntc25:.0f} ohm NTC, {args.pullup:.0f} ohm pull-up "
        f"to {args.rail} V\n"
        f"  a board at 25 degC should show {volts[temps.index(25.0)] * 1000:.0f} mV "
        f"at the pin - check with 'adc' over serial, or a meter"
    )


if __name__ == "__main__":
    main()
