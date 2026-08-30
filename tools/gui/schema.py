#!/usr/bin/env python3
"""What the browser is told about the stream, so no field name is hardcoded in JS.

The set of fields comes from console.STREAM, which comes from Src/app/stream.c.
This file only *annotates* them - label, unit, scale, colour, which panel they
belong on, which channel they describe. An id added to the firmware and to
console.STREAM appears in the GUI without touching this file or the JavaScript;
adding an entry here only gives it a proper name and unit.

Multiple channels land the same way: a field carries `channel`, the browser
groups panels and the V-I plot by it, and a second channel's ids need only an
entry each.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field as dc_field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import console  # noqa: E402  - the field list itself

# Same palette as the matplotlib tools, so a live trace and a rendered SVG of
# the same quantity are the same colour.
VIN_C, IIN_C, PIN_C, VBUS_C, DUTY_C, FLAG_C, TARGET_C = (
    "#ff7f0e", "#9467bd", "#2ca02c", "#1f77b4", "#d62728", "#9aa4b2", "#8c564b")
FALLBACK_C = ("#17becf", "#bcbd22", "#e377c2", "#8c564b", "#7f7f7f")


@dataclass(frozen=True)
class Field:
    label: str
    unit: str
    scale: float          # display value = wire value / scale
    colour: str
    panel: str
    channel: str | None = None
    step: bool = False    # drawn as steps-post; a commanded value, not a measured one
    kind: str = "number"  # "bits" gets decoded into chips instead of read as a number
    digits: int = 3


# Annotations for the ids in console.STREAM. Unknown ids still plot.
FIELD_INFO: dict[str, Field] = {
    "vbus_mv": Field("vbus", "V", 1000.0, VBUS_C, "volts", None, digits=2),
    "vin_mv": Field("vin", "V", 1000.0, VIN_C, "volts", "a"),
    "iin_ma": Field("iin", "A", 1000.0, IIN_C, "current", "a"),
    "vin_target_mv": Field("vin target", "V", 1000.0, TARGET_C, "volts", "a",
                           step=True),
    "duty": Field("duty", "/1000", 1.0, DUTY_C, "duty", "a", step=True, digits=0),
    "flags": Field("flags", "", 1.0, FLAG_C, "flags", "a", step=True, kind="bits",
                   digits=0),
}


@dataclass(frozen=True)
class Derived:
    """A value the board does not send, computed from ones it does.

    Only published when every input is present in console.STREAM, so removing a
    packet from the firmware removes what depends on it rather than plotting a
    silent zero.
    """
    key: str
    label: str
    unit: str
    colour: str
    panel: str
    inputs: tuple[str, ...]
    expr: str                      # JS-free: evaluated server-side by name
    channel: str | None = None
    digits: int = 2
    args: tuple = dc_field(default=(), repr=False)


DERIVED: tuple[Derived, ...] = (
    Derived("pin_w", "pin", "W", PIN_C, "power", ("vin_mv", "iin_ma"),
            expr="product_milli", channel="a"),
)

# key -> callable over the wire values of Derived.inputs.
EXPRESSIONS = {
    "product_milli": lambda a, b: a * b / 1_000_000.0,   # mV * mA -> W
}

# Panel order and titles. A field naming a panel not listed here gets its own,
# appended in first-seen order.
PANELS: tuple[tuple[str, str], ...] = (
    ("volts", "voltage (V)"),
    ("current", "current (A)"),
    ("power", "power (W)"),
    ("duty", "duty (/1000)"),
    ("flags", "flags"),
)

# Src/app/stream.c: telem_a.valid, then PWM_STATE_RUNNING.
FLAG_BITS: tuple[tuple[int, str], ...] = (
    (0x01, "telem"),
    (0x02, "running"),
)

# Which fields the V-I plane uses, per channel. The persistence plot is the one
# place a pair of fields means something together rather than separately.
IV_PAIRS: dict[str, dict[str, str]] = {
    "a": {"x": "iin_ma", "y": "vin_mv", "power": "pin_w"},
}


def fields() -> list[dict]:
    """Every plottable field, streamed then derived, in wire order."""
    out: list[dict] = []
    spare = iter(FALLBACK_C)
    for ident in sorted(console.STREAM):
        name, width, signed = console.STREAM[ident]
        info = FIELD_INFO.get(name)
        if info is None:
            # An id the firmware gained that nobody has annotated yet. Plot it
            # raw rather than dropping it - a missing trace is harder to notice
            # than an unlabelled one.
            info = Field(name, "", 1.0, next(spare, "#888888"), "other", None,
                         digits=0)
        out.append({"key": name, "id": ident, "width": width, "signed": signed,
                    "derived": False, **_as_json(info)})

    have = {f["key"] for f in out}
    for der in DERIVED:
        if not set(der.inputs) <= have:
            continue
        out.append({"key": der.key, "id": None, "derived": True,
                    "inputs": list(der.inputs), "label": der.label,
                    "unit": der.unit, "scale": 1.0, "colour": der.colour,
                    "panel": der.panel, "channel": der.channel, "step": False,
                    "kind": "number", "digits": der.digits})

    # Panel order, not wire order: the readout strip reads vin/iin/pin before
    # vbus/duty/flags, which is the order the quantities are thought about in.
    order = [name for name, _ in PANELS]
    out.sort(key=lambda f: order.index(f["panel"]) if f["panel"] in order
             else len(order))
    return out


def _as_json(info: Field) -> dict:
    return {"label": info.label, "unit": info.unit, "scale": info.scale,
            "colour": info.colour, "panel": info.panel, "channel": info.channel,
            "step": info.step, "kind": info.kind, "digits": info.digits}


def panels(all_fields: list[dict]) -> list[dict]:
    order = [name for name, _ in PANELS]
    titles = dict(PANELS)
    seen: list[str] = []
    for f in all_fields:
        if f["panel"] not in seen:
            seen.append(f["panel"])
    seen.sort(key=lambda p: order.index(p) if p in order else len(order))
    return [{"id": p, "label": titles.get(p, p),
             "fields": [f["key"] for f in all_fields if f["panel"] == p]}
            for p in seen]


def active_derived() -> tuple[Derived, ...]:
    """Derived fields whose inputs the firmware actually sends."""
    have = {console.STREAM[i][0] for i in console.STREAM}
    return tuple(d for d in DERIVED if set(d.inputs) <= have)


def build(sequences: list[dict]) -> dict:
    """The whole contract handed to the browser on connect."""
    all_fields = fields()
    channels = []
    for f in all_fields:
        if f["channel"] and f["channel"] not in channels:
            channels.append(f["channel"])
    return {
        "fields": all_fields,
        "panels": panels(all_fields),
        "channels": channels or ["a"],
        "iv": {ch: pair for ch, pair in IV_PAIRS.items() if ch in channels},
        "flag_bits": [{"mask": m, "label": l} for m, l in FLAG_BITS],
        "commands": sorted(console.OPCODES),
        "sequences": sequences,
        "stream_period_ms": console.STREAM_PERIOD_MS,
        "baud": console.BAUD,
        "max_payload": console.MAX_PAYLOAD,
    }
