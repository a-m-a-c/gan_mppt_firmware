#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""Web GUI for live MPPT telemetry and PWM control.

Reads the CSV stream from the firmware, serves a local page that plots any
selection of series over a chosen time span, and sends PWM commands back the
other way over the same link.

Usage (uv resolves pyserial from the inline metadata above - no venv needed):
    uv run tools/telem_gui.py                  # auto-detect port, open browser
    uv run tools/telem_gui.py -p COM6          # pick the port explicitly
    uv run tools/telem_gui.py --port-http 8080 # serve somewhere else
    uv run tools/telem_gui.py --read-only      # plot only, no PWM control
    uv run tools/telem_gui.py --list           # show candidate serial ports

Telemetry line format (23 integer fields):
    <tick_ms>,<valid_mask>,<vbus_mv>,
    <ch1_vin_mv>,<ch1_iin_ma>,<ch1_vout_mv>,<ch1_iout_ma>,  ... through ch5

Everything else the firmware sends starts with '#' - see Inc/drivers/command.h
for the command grammar and the reply lines parsed below.

The PWM panel drives a live power stage. Nothing is energised until Start is
pressed, and Stop All is always one click away - but note that the duty slider
applies as it moves, on a running channel included.
"""

import argparse
import json
import sys
import threading
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required. Run via 'uv run tools/telem_gui.py', "
             "which installs it from the inline metadata above.")

BAUD = 115200
FIELD_COUNT = 23
HISTORY = 24000          # ~20 min at 20 Hz
LIKELY = ("cp210", "ch340", "ft232", "ftdi", "usb serial", "silicon labs")

CHANNEL_COUNT = 5
CHANNEL_NAMES = ["A", "B", "C", "D", "E"]

# pwm_state_t, in order. Kept in step with Inc/drivers/pwm.h.
STATE_NAMES = ["uninit", "stopped", "running", "faulted"]

# Firmware units and limits, mirroring the PWM_MIN/MAX defines in Inc/config.h.
# The
# page enforces these before sending so an out-of-range value is caught here
# rather than costing a round trip - the firmware re-checks regardless.
LIMITS = {
    "freq_hz": [100000, 800000],
    "duty_tenths": [100, 900],
    "dead_ns": [5, 300],
}

# Longest command line the firmware will assemble (SERIAL_CMD_MAX), less its
# terminator.
COMMAND_MAX = 95

# Series order must match the firmware line. (label, unit, scale-from-integer)
SERIES = [("V_BUS", "V", 1000.0)]
for _ch in range(1, 6):
    SERIES += [
        (f"CH{_ch} Vin", "V", 1000.0),
        (f"CH{_ch} Iin", "A", 1000.0),
        (f"CH{_ch} Vout", "V", 1000.0),
        (f"CH{_ch} Iout", "A", 1000.0),
    ]

# Shared between the reader thread and the HTTP handlers.
_lock = threading.Lock()
_samples = deque(maxlen=HISTORY)   # (seq, t_seconds, [values...])
_seq = 0
_status = {"connected": False, "port": None, "error": None, "valid_mask": 0}

# Latest "#cfg" report per channel (1..5), and the recent reply lines. The log
# is short and sent whole on every poll rather than diffed - it is a handful of
# strings, and replacing it wholesale cannot drift out of sync.
_config = {}
_events = deque(maxlen=12)

# The open port, so the HTTP threads can write commands to it. Only the reader
# thread assigns it; None means "not connected right now".
_serial = None
_write_lock = threading.Lock()
_read_only = False


def find_port():
    ports = list(list_ports.comports())
    if not ports:
        return None
    for p in ports:
        haystack = f"{p.description} {p.manufacturer or ''}".lower()
        if any(k in haystack for k in LIKELY):
            return p.device
    return ports[0].device if len(ports) == 1 else None


def show_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"  {p.device:10s}  {p.description}")


def parse(line):
    """Return (tick_ms, valid_mask, [scaled values]) or None if not telemetry."""
    parts = line.split(",")
    if len(parts) != FIELD_COUNT:
        return None
    try:
        raw = [int(p) for p in parts]
    except ValueError:
        return None  # partial first line, or noise on connect
    tick_ms, valid_mask = raw[0], raw[1]
    values = [raw[2 + i] / SERIES[i][2] for i in range(len(SERIES))]
    return tick_ms, valid_mask, values


def log_event(text):
    with _lock:
        _events.append(text)


def handle_report(line):
    """Consume one '#' line from the firmware: a config report or a reply."""
    parts = line[1:].split(",")
    if parts[0] == "cfg" and len(parts) == 6:
        try:
            channel, state, freq, duty, dead = (int(p) for p in parts[1:])
        except ValueError:
            return                      # noise that happens to start with '#cfg'
        if 1 <= channel <= CHANNEL_COUNT:
            with _lock:
                _config[channel] = {"state": state, "freq_hz": freq,
                                    "duty_tenths": duty, "dead_ns": dead}
        return
    # "#ok,..." / "#err,..." and anything unrecognised: show it to the operator
    # rather than dropping it, since it is always about a command they sent.
    log_event(line)


def send_command(text):
    """Write one command line. Returns None on success, else an error string."""
    text = text.strip()
    if _read_only:
        return "started with --read-only"
    if not text:
        return "empty command"
    if len(text) > COMMAND_MAX:
        return "command too long"
    if any(ch < " " or ch > "~" for ch in text):
        return "command must be printable ASCII"

    with _lock:
        ser = _serial
    if ser is None:
        return "not connected"

    try:
        # One writer at a time: a half-written line would be parsed as a
        # malformed command rather than being merged with the next one.
        with _write_lock:
            ser.write((text + "\n").encode("ascii"))
    except (serial.SerialException, OSError) as exc:
        return str(exc)

    log_event("> " + text)
    return None


def reader(port, baud):
    """Serial reader thread. Reconnects on failure rather than dying."""
    global _seq, _serial
    while True:
        try:
            with serial.Serial(port, baud, timeout=1) as ser:
                with _lock:
                    _serial = ser
                    _status.update(connected=True, port=port, error=None)
                while True:
                    line = ser.readline().decode("ascii", errors="replace").strip()
                    if not line:
                        continue
                    if line.startswith("#"):
                        handle_report(line)
                        continue
                    parsed = parse(line)
                    if parsed is None:
                        continue
                    tick_ms, valid_mask, values = parsed
                    with _lock:
                        _seq += 1
                        _samples.append((_seq, tick_ms / 1000.0, values))
                        _status["valid_mask"] = valid_mask
        except serial.SerialException as exc:
            with _lock:
                _status.update(connected=False, error=str(exc))
            # Wait before retrying so a missing port does not spin the CPU.
            threading.Event().wait(2.0)
        finally:
            # Drop the handle before anything can write to a closed port; the
            # stale config stays on screen, greyed out by the status dot.
            with _lock:
                _serial = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # keep the console clean for the operator

    def _send(self, code, body, content_type):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        route = urlparse(self.path)
        if route.path == "/":
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif route.path == "/series":
            self._send(200, json.dumps([{"label": s[0], "unit": s[1]} for s in SERIES]),
                       "application/json")
        elif route.path == "/data":
            since = int((parse_qs(route.query).get("since") or ["0"])[0])
            with _lock:
                rows = [[s, t, v] for (s, t, v) in _samples if s > since]
                status = dict(_status)
                config = dict(_config)
                events = list(_events)
            status["read_only"] = _read_only
            self._send(200, json.dumps({"status": status, "rows": rows,
                                        "config": config, "events": events,
                                        "limits": LIMITS}),
                       "application/json")
        else:
            self._send(404, "not found", "text/plain")

    def do_POST(self):
        """/cmd takes {"cmds": [...]} and forwards each line to the firmware.

        Commands go in order and stop at the first failure, so a partly
        applied batch is reported as an error rather than passing silently.
        The reply is only about reaching the board - what the firmware made of
        each line comes back on the '#' lines, in the event log."""
        if urlparse(self.path).path != "/cmd":
            self._send(404, "not found", "text/plain")
            return

        try:
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8"))
            commands = body["cmds"]
            if not isinstance(commands, list) or not all(isinstance(c, str) for c in commands):
                raise ValueError("cmds must be a list of strings")
        except (ValueError, KeyError, UnicodeDecodeError) as exc:
            self._send(400, json.dumps({"error": f"bad request: {exc}"}),
                       "application/json")
            return

        error = None
        for command in commands:
            error = send_command(command)
            if error:
                break
        self._send(200, json.dumps({"error": error}), "application/json")


PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8"><title>MPPT Telemetry</title>
<style>
  :root { --bg:#f2f4f7; --panel:#ffffff; --line:#d4dae2; --fg:#1b2330; --dim:#666f7d;
          --hover:#eceff4; --font:Verdana,Geneva,DejaVu Sans,sans-serif; }
  * { box-sizing: border-box; }
  body { margin:0; height:100vh; display:flex; background:var(--bg); color:var(--fg);
         font:12px/1.5 var(--font); }
  #side { width:250px; flex:none; background:var(--panel); border-right:1px solid var(--line);
          padding:14px; overflow-y:auto; }
  #main { flex:1; display:flex; flex-direction:column; padding:14px; min-width:0; }
  h2 { margin:0 0 4px; font-size:13px; }
  .grp { margin:14px 0 7px; color:var(--dim); font-size:10px; text-transform:uppercase;
         letter-spacing:.08em; }
  .bubs { display:flex; flex-wrap:wrap; gap:6px; }
  /* Bubble toggles: filled when the series is plotted, hollow when it is not.
     The border always carries the series colour so the mapping to the trace
     stays readable in both states. */
  .bub { display:inline-flex; align-items:center; gap:6px; padding:4px 11px;
         border:1.5px solid; cursor:pointer; user-select:none;
         font:11px var(--font); transition:background .12s, color .12s; }
  .bub .dotc { width:7px; height:7px; border-radius:50%; flex:none; background:currentColor; }
  .bub.off { background:transparent; }
  .bub.off:hover { background:var(--hover); }
  select, button, input { background:#fff; color:var(--fg); border:1px solid var(--line);
                          padding:5px 8px; font:12px var(--font); }
  button { cursor:pointer; }
  button:hover { background:var(--hover); }
  input[type=number] { width:58px; }
  /* The track draws its own frame, so the shared input border would double it. */
  input[type=range] { width:90px; padding:0; border:none; background:transparent;
                      vertical-align:middle; cursor:pointer; }

  /* Per-channel PWM state, beside the series list so it stays in view while
     the plot has the floor. */
  #chstat { margin-top:8px; }
  #chstat div { display:flex; align-items:center; gap:8px; padding:3px 0; }
  #chstat b { flex:none; width:58px; }
  /* Paused is a mode, not a momentary action, so the button holds its state. */
  #pause.on { background:#ffe6c2; border-color:#dc9b3c; color:#8a4f00; }
  #bar { display:flex; gap:10px; align-items:center; margin-bottom:10px; flex-wrap:wrap; }
  #stat { margin-left:auto; color:var(--dim); }
  #wrap { flex:1; position:relative; border:1px solid var(--line);
          background:var(--panel); min-height:0; }
  canvas { position:absolute; inset:0; width:100%; height:100%; }
  .dot { display:inline-block; width:7px; height:7px; border-radius:50%; margin-right:5px; }

  /* The control row is wide; scroll it inside the panel rather than letting it
     push the whole page sideways on a narrow window. */
  #pwm { margin-top:10px; border:1px solid var(--line); background:var(--panel);
         padding:10px 12px 12px; flex:none; overflow-x:auto; }
  /* max-content plus nowrap: the row keeps its natural width and the panel
     scrolls, instead of the browser wrapping controls onto a second line. */
  #pwm table { border-collapse:collapse; width:max-content; }
  #pwm th, #pwm td { white-space:nowrap; }
  #pwm th { text-align:left; font-weight:normal; color:var(--dim); font-size:10px;
            text-transform:uppercase; letter-spacing:.08em; padding:0 10px 5px 0; }
  #pwm td { padding:4px 6px 4px 0; border-top:1px solid var(--line); }
  #pwm td.ch { font-weight:bold; }
  #pwm button { padding:4px 8px; }
  #pwm button.step { padding:4px 4px; font:11px var(--font); }
  /* An edited field stays highlighted until it is applied, so a value on
     screen is never mistaken for a value in force. */
  input.dirty { border-color:#dc9b3c; background:#fff7e9; }
  .state { display:inline-block; min-width:50px; text-align:center; padding:2px 8px;
           font-size:10px; text-transform:uppercase; letter-spacing:.05em; }
  .state.uninit  { background:#e7eaef; color:#5c6572; }
  .state.stopped { background:#dce6f5; color:#1f4d80; }
  .state.running { background:#d8efd8; color:#1a6b1a; }
  .state.faulted { background:#fadbdb; color:#992222; }
  button.go   { border-color:#2ca02c; color:#186418; }
  button.halt { border-color:#d62728; color:#992222; }
  #pwmfoot { display:flex; gap:10px; align-items:center; margin-top:10px; }
  #stopall { background:#d62728; border-color:#a01a1b; color:#fff; font-weight:bold; }
  #stopall:hover { background:#bc1f20; }
  #pwmnote { color:#992222; }
  #pwmlog { margin-top:8px; max-height:76px; overflow-y:auto; white-space:pre-wrap;
            font:11px/1.45 ui-monospace,Consolas,DejaVu Sans Mono,monospace;
            color:var(--dim); }
</style></head><body>
<div id="side">
  <h2>Series</h2><div id="list"></div>
  <div class="grp" style="margin-top:18px">Channel state</div>
  <div id="chstat"></div>
</div>
<div id="main">
  <div id="bar">
    <label style="gap:6px">Span
      <select id="span">
        <option value="10">10 s</option>
        <option value="30" selected>30 s</option>
        <option value="60">1 min</option>
        <option value="300">5 min</option>
      </select>
    </label>
    <label style="gap:6px">Average
      <input id="avg" type="number" min="1" max="500" step="1" value="1" title="moving-average window, in samples (1 = off)">
      samples
    </label>
    <button id="pause">Pause</button>
    <button id="clear">Clear</button>
    <span id="stat"></span>
  </div>
  <div id="wrap"><canvas id="cv"></canvas></div>
  <div id="pwm">
    <table>
      <thead><tr>
        <th>Channel</th><th>State</th><th>Frequency (kHz)</th><th>Duty (%)</th>
        <th>Dead time (ns)</th><th>Duty adjust</th><th></th><th></th>
      </tr></thead>
      <tbody id="pwmrows"></tbody>
    </table>
    <div id="pwmfoot">
      <button id="stopall">STOP ALL</button>
      <button id="clearovp">Clear OVP</button>
      <span id="pwmnote"></span>
    </div>
    <div id="pwmlog"></div>
  </div>
</div>
<script>
// Saturated rather than pastel - these have to hold up as thin lines on white.
const COLORS = ["#1f77b4","#2ca02c","#e6550d","#d62728","#7b52ab","#0f9aa8",
                "#8c564b","#c2379a","#5a6570","#9a9c1c","#3182bd","#31a354",
                "#a63603","#843c39","#756bb1","#1b7f7f","#7b4173","#5254a3",
                "#7d8f2f","#b5651d","#4a4a9c"];
let series = [], on = new Set(), rows = [], since = 0;
let paused = false, frozenEnd = null;
const cv = document.getElementById("cv"), ctx = cv.getContext("2d");

fetch("/series").then(r => r.json()).then(s => {
  series = s;
  const list = document.getElementById("list");
  let group = "", row = null;
  s.forEach((it, i) => {
    const g = it.label.startsWith("CH") ? it.label.slice(0, 3) : "Bus";
    if (g !== group) {
      group = g;
      const h = document.createElement("div");
      h.className = "grp"; h.textContent = g; list.appendChild(h);
      row = document.createElement("div"); row.className = "bubs"; list.appendChild(row);
    }
    const colour = COLORS[i % COLORS.length];
    const bub = document.createElement("span");
    bub.className = "bub";
    const dot = document.createElement("span");
    dot.className = "dotc";
    // Strip the "CHn " prefix - the group heading already says which channel.
    bub.append(dot, document.createTextNode(it.label.replace(/^CH\d /, "")));
    bub.style.borderColor = colour;
    const paint = () => {
      const active = on.has(i);
      bub.classList.toggle("off", !active);
      bub.style.background = active ? colour : "transparent";
      bub.style.color = active ? "#fff" : colour;
    };
    bub.onclick = () => { on.has(i) ? on.delete(i) : on.add(i); paint(); draw(); };
    if (i === 0) on.add(0);
    paint();
    row.appendChild(bub);
  });
  draw();
});

document.getElementById("span").onchange = draw;
document.getElementById("avg").oninput = draw;
document.getElementById("clear").onclick = () => { rows = []; frozenEnd = null; draw(); };

// Pause freezes the view only - samples keep arriving in the background, so
// resuming jumps to live rather than replaying a backlog.
const pauseBtn = document.getElementById("pause");
pauseBtn.onclick = () => {
  paused = !paused;
  frozenEnd = paused && rows.length ? rows[rows.length - 1][0] : null;
  pauseBtn.textContent = paused ? "Resume" : "Pause";
  pauseBtn.classList.toggle("on", paused);
  draw();
};

// ---- PWM control ---------------------------------------------------------
// The firmware works in Hz, tenths of a percent and ns; the page shows kHz, %
// and ns. Converting only at the edges keeps every comparison against the
// reported config exact, with no float drift in the middle.
const CHNAMES = ["A", "B", "C", "D", "E"];
const STATES = ["uninit", "stopped", "running", "faulted"];
const FIELDS = [
  { id: "f", cmd: "freq", key: "freq_hz",     unit: "kHz", scale: 1000, step: "0.1" },
  { id: "d", cmd: "duty", key: "duty_tenths", unit: "%",   scale: 10,   step: "0.1" },
  { id: "t", cmd: "dt",   key: "dead_ns",     unit: "ns",  scale: 1,    step: "1" },
];
let cfg = {}, limits = null, pwmLive = null, roNoted = false;

// The duty slider applies as it moves, so sends are throttled to one per
// SLIDE_MS with the final position always sent - an unthrottled drag would
// queue hundreds of command lines into a 115200-baud link. For HOLD_MS after
// a move, config reports leave the duty field and slider alone, so the value
// under the cursor is never yanked back by a report that predates it.
const SLIDE_MS = 120, HOLD_MS = 500;
const slide = {};

const pwmPanel = document.getElementById("pwm");
const pwmRows = document.getElementById("pwmrows");
const pwmNote = document.getElementById("pwmnote");
const chStat = document.getElementById("chstat");

// delta is in the firmware's own units, tenths of a percent.
function stepButton(n, delta) {
  return '<button class="step" data-ch="' + n + '" data-act="step" data-delta="' +
         delta + '" disabled>' + (delta > 0 ? "+" : "&minus;") +
         Math.abs(delta) / 10 + "%</button>";
}

for (let n = 1; n <= 5; n++) {
  let html = '<td class="ch">CH' + n + " &middot; " + CHNAMES[n - 1] + "</td>" +
             '<td><span class="state uninit" id="st' + n + '">-</span></td>';
  // The units live in the column headings, not beside every field - five rows
  // of "kHz / % / ns" is width the controls need more.
  for (const f of FIELDS)
    html += '<td><input id="' + f.id + n + '" type="number" step="' + f.step +
            '" disabled></td>';
  // Down on the left of the slider, up on the right, so the buttons move the
  // thumb the way they read.
  html += "<td>" + stepButton(n, -10) + stepButton(n, -1) +
          ' <input id="s' + n + '" data-ch="' + n + '" type="range" step="1" ' +
          'min="100" max="900" title="applies as it moves" disabled> ' +
          stepButton(n, 1) + stepButton(n, 10) + "</td>" +
          '<td><button data-ch="' + n + '" data-act="apply" disabled>Apply</button> ' +
          '<button data-ch="' + n + '" data-act="init" disabled>Init</button></td>' +
          '<td><button class="go" data-ch="' + n + '" data-act="start" disabled>Start</button> ' +
          '<button class="halt" data-ch="' + n + '" data-act="stop" disabled>Stop</button> ' +
          '<button data-ch="' + n + '" data-act="clear" disabled>Clear fault</button></td>';
  const tr = document.createElement("tr");
  tr.innerHTML = html;
  pwmRows.appendChild(tr);

  const line = document.createElement("div");
  line.innerHTML = "<b>CH" + n + " &middot; " + CHNAMES[n - 1] + "</b>" +
                   '<span class="state uninit" id="sst' + n + '">-</span>';
  chStat.appendChild(line);
}

function note(text) { pwmNote.textContent = text; }

// Editing a field marks it, which both highlights it and stops the incoming
// config reports from overwriting what is being typed. The slider is the
// exception: it applies itself, so it never goes dirty.
pwmRows.addEventListener("input", ev => {
  const el = ev.target;
  if (el.tagName !== "INPUT") return;
  if (el.type === "range") slideDuty(+el.dataset.ch, +el.value);
  else el.classList.add("dirty");
});

function sliding(n) {
  return slide[n] && (Date.now() - slide[n].touched) < HOLD_MS;
}

// One step of the buttons either side of the slider. Inside the hold window
// the step is taken from the last value we asked for, not from the reported
// one, so a run of quick clicks accumulates instead of fighting a report that
// has not caught up yet.
function stepDuty(n, delta) {
  const [lo, hi] = limits ? limits.duty_tenths : [100, 900];
  const base = sliding(n) ? slide[n].target
                          : +document.getElementById("s" + n).value;
  slideDuty(n, Math.min(hi, Math.max(lo, base + delta)));
}

function slideDuty(n, tenths) {
  const field = document.getElementById("d" + n);
  field.value = String(tenths / 10);
  field.classList.remove("dirty");   // the control is applying it, not the operator
  document.getElementById("s" + n).value = tenths;

  const s = slide[n] || (slide[n] = { timer: null, pending: null });
  s.touched = Date.now();
  s.target = tenths;
  s.pending = tenths;
  if (s.timer) return;               // a send is already in flight for this row

  const fire = () => {
    const value = s.pending;
    s.pending = null;
    if (value === null) { s.timer = null; return; }
    send(["set " + n + " duty " + value]).then(() => { s.touched = Date.now(); });
    s.timer = setTimeout(fire, SLIDE_MS);
  };
  fire();
}

pwmRows.addEventListener("click", ev => {
  const btn = ev.target.closest("button");
  if (!btn) return;
  const n = +btn.dataset.ch;
  if (btn.dataset.act === "apply") applyRow(n);
  else if (btn.dataset.act === "step") stepDuty(n, +btn.dataset.delta);
  else send([btn.dataset.act + " " + n]);
});

document.getElementById("stopall").onclick = () => send(["stop all"]);
document.getElementById("clearovp").onclick = () => send(["clear ovp"]);

// Returns the error string, or "" if the whole batch reached the board. What
// the firmware made of each line comes back separately, in the log.
async function send(cmds) {
  note("");
  try {
    const r = await fetch("/cmd", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ cmds }),
    });
    const d = await r.json();
    if (d.error) note(d.error);
    return d.error || "";
  } catch (e) { note(String(e)); return String(e); }
}

async function applyRow(n) {
  if (!limits) return;
  const current = cfg[n];
  const cmds = [];

  for (const f of FIELDS) {
    const el = document.getElementById(f.id + n);
    const value = Math.round(parseFloat(el.value) * f.scale);
    if (!isFinite(value)) { note("CH" + n + " " + f.cmd + ": not a number"); return; }
    const [lo, hi] = limits[f.key];
    if (value < lo || value > hi) {
      note("CH" + n + " " + f.cmd + ": " + lo / f.scale + " to " + hi / f.scale +
           " " + f.unit);
      return;
    }
    // Only changed fields are sent. Re-applying a frequency or dead time stops
    // and restarts a running channel, so a no-op Apply would glitch a live
    // output for nothing.
    if (!current || value !== current[f.key])
      cmds.push("set " + n + " " + f.cmd + " " + value);
  }

  if (!cmds.length) { note("CH" + n + ": already set"); clearDirty(n); return; }
  if (!await send(cmds)) clearDirty(n);
}

// Dropping the mark hands the field back to the config reports, so a value the
// firmware rejected visibly snaps back to what is really in force.
function clearDirty(n) {
  for (const f of FIELDS)
    document.getElementById(f.id + n).classList.remove("dirty");
}

function setField(id, value) {
  const el = document.getElementById(id);
  if (el === document.activeElement || el.classList.contains("dirty")) return;
  const text = String(Math.round(value * 10) / 10);
  if (el.value !== text) el.value = text;
}

function setPwmEnabled(live) {
  for (const el of pwmPanel.querySelectorAll("input, button")) el.disabled = !live;
}

function paintState(id, c) {
  const badge = document.getElementById(id);
  const name = (c && STATES[c.state]) ? STATES[c.state] : "uninit";
  badge.className = "state " + name;
  badge.textContent = c ? name : "-";
}

function updatePwm(d) {
  cfg = d.config || {};
  limits = d.limits;

  const live = d.status.connected && !d.status.read_only;
  if (live !== pwmLive) { pwmLive = live; setPwmEnabled(live); }
  if (d.status.read_only && !roNoted) { roNoted = true; note("read-only session"); }

  for (let n = 1; n <= 5; n++) {
    const c = cfg[n];
    paintState("st" + n, c);    // in the control row
    paintState("sst" + n, c);   // and beside the series list
    if (!c) continue;

    const slider = document.getElementById("s" + n);
    if (limits) {
      slider.min = limits.duty_tenths[0];
      slider.max = limits.duty_tenths[1];
    }
    for (const f of FIELDS)
      if (f.key !== "duty_tenths" || !sliding(n)) setField(f.id + n, c[f.key] / f.scale);
    if (!sliding(n)) slider.value = c.duty_tenths;
  }

  document.getElementById("pwmlog").textContent = (d.events || []).join("\n");
}

async function poll() {
  try {
    const r = await fetch("/data?since=" + since);
    const d = await r.json();
    for (const [seq, t, vals] of d.rows) { since = seq; rows.push([t, vals]); }
    if (rows.length > 30000) rows.splice(0, rows.length - 30000);
    updatePwm(d);
    const st = d.status;
    document.getElementById("stat").innerHTML =
      '<span class="dot" style="background:' + (st.connected ? "#2ca02c" : "#d62728") + '"></span>' +
      (st.connected ? (st.port + " &middot; " + rows.length + " samples")
                    : ("disconnected" + (st.error ? " &middot; " + st.error : ""))) +
      (paused ? ' &middot; <b>paused</b>' : "");
    draw();
  } catch (e) { /* server gone; next tick retries */ }
  setTimeout(poll, 250);
}
poll();

// Round a raw interval up to the next 1, 2, 2.5 or 5 x 10^n, so gridlines land
// on values a human would have chosen.
function niceStep(x) {
  const mag = Math.pow(10, Math.floor(Math.log10(Math.abs(x) || 1)));
  const n = x / mag;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 2.5 ? 2.5 : n <= 5 ? 5 : 10) * mag;
}

// Enough decimals to distinguish adjacent gridlines, and no more. The extra
// place is for 2.5-style steps, which the magnitude alone would round away.
function fmt(v, step) {
  let d = Math.max(0, -Math.floor(Math.log10(step)));
  if (Math.abs((step * Math.pow(10, d)) % 1) > 1e-9) d += 1;
  return v.toFixed(d);
}

function draw() {
  const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
  cv.width = w * dpr; cv.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (!rows.length || !on.size) return;

  const span = +document.getElementById("span").value;
  const N = Math.max(1, Math.min(500,
    Math.round(+document.getElementById("avg").value) || 1));

  // While paused the window end is pinned, so incoming samples accumulate off
  // the right edge instead of scrolling the view.
  const tEnd = (paused && frozenEnd !== null) ? frozenEnd : rows[rows.length - 1][0];
  const tStart = tEnd - span;

  let i0 = rows.findIndex(r => r[0] >= tStart);
  if (i0 < 0) i0 = rows.length;
  let iEnd = rows.length;
  if (paused && frozenEnd !== null) {
    const after = rows.findIndex(r => r[0] > tEnd);
    if (after >= 0) iEnd = after;
  }

  // Take N-1 samples of lead-in before the window so the leftmost points are
  // averaged over a full window rather than ramping up from a short one.
  const from = Math.max(0, i0 - (N - 1));
  const seg = rows.slice(from, iEnd);
  const lead = i0 - from;
  if (seg.length <= lead) return;

  // One smoothed trace per selected series, computed once and reused for both
  // autoscaling and drawing so the axes always describe what is on screen.
  const trace = {};
  for (const i of on) {
    const pts = [];
    const q = [];
    let sum = 0;
    for (let k = 0; k < seg.length; k++) {
      const v = seg[k][1][i];
      q.push(v); sum += v;
      if (q.length > N) sum -= q.shift();
      if (k >= lead) pts.push([seg[k][0], sum / q.length]);
    }
    trace[i] = pts;
  }

  const L = 70, R = 70, T = 14, B = 28, pw = w - L - R, ph = h - T - B;
  if (pw <= 0 || ph <= 0) return;

  // Volts on the left axis, amps on the right - mixing them on one scale
  // makes both unreadable.
  const extent = {};
  for (const u of ["V", "A"]) {
    let lo = Infinity, hi = -Infinity;
    for (const i of on) {
      if (series[i].unit !== u) continue;
      for (const [, v] of trace[i]) { if (v < lo) lo = v; if (v > hi) hi = v; }
    }
    extent[u] = (lo === Infinity) ? null : [lo, hi];
  }

  // Snap both axes to 1/2/2.5/5 x 10^n steps, then give them a common division
  // count so the two label columns share one set of horizontal gridlines.
  const axis = {};
  for (const u of ["V", "A"]) {
    if (!extent[u]) { axis[u] = null; continue; }
    let [lo, hi] = extent[u];
    if (hi - lo < 1e-9) { hi += 0.5; lo -= 0.5; }
    const step = niceStep((hi - lo) / 5);
    const base = Math.floor(lo / step) * step;
    axis[u] = { step, base, divs: Math.max(1, Math.ceil((hi - base) / step)) };
  }
  const divs = Math.max(axis.V ? axis.V.divs : 1, axis.A ? axis.A.divs : 1);
  const rng = {};
  for (const u of ["V", "A"])
    rng[u] = axis[u] ? [axis[u].base, axis[u].base + axis[u].step * divs] : null;

  const X = t => L + ((t - tStart) / span) * pw;
  const Y = (v, u) => { const [a, b] = rng[u]; return T + ph - ((v - a) / (b - a)) * ph; };

  ctx.fillStyle = "#5c6572";
  ctx.font = "11px Verdana,Geneva,sans-serif"; ctx.lineWidth = 1;

  for (let i = 0; i <= divs; i++) {
    const y = T + ph - (ph * i) / divs;
    ctx.strokeStyle = (i === 0) ? "#aab3c0" : "#e4e8ee";
    ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + pw, y); ctx.stroke();
    if (axis.V) { ctx.textAlign = "right";
      ctx.fillText(fmt(axis.V.base + axis.V.step * i, axis.V.step) + " V", L - 8, y + 4); }
    if (axis.A) { ctx.textAlign = "left";
      ctx.fillText(fmt(axis.A.base + axis.A.step * i, axis.A.step) + " A", L + pw + 8, y + 4); }
  }

  // Vertical gridlines land on round time offsets rather than on fractions of
  // the span, so they stay put as samples arrive.
  const tStep = niceStep(span / 6);
  ctx.textAlign = "center";
  for (let t = Math.ceil(tStart / tStep) * tStep; t <= tEnd + 1e-9; t += tStep) {
    const x = X(t);
    ctx.strokeStyle = "#e4e8ee";
    ctx.beginPath(); ctx.moveTo(x, T); ctx.lineTo(x, T + ph); ctx.stroke();
    ctx.fillText(fmt(t - tEnd, tStep) + "s", x, h - 8);
  }

  ctx.lineWidth = 1.6;
  for (const i of on) {
    if (!rng[series[i].unit]) continue;
    ctx.strokeStyle = COLORS[i % COLORS.length];
    ctx.beginPath();
    trace[i].forEach(([t, v], k) => {
      const x = X(t), y = Y(v, series[i].unit);
      k ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.stroke();
  }
}
window.addEventListener("resize", draw);
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser(description="Web GUI for MPPT telemetry.")
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("-b", "--baud", type=int, default=BAUD, help=f"baud (default {BAUD})")
    ap.add_argument("--port-http", type=int, default=8000, help="HTTP port (default 8000)")
    ap.add_argument("--no-browser", action="store_true", help="do not open a browser")
    ap.add_argument("--read-only", action="store_true",
                    help="hide the PWM panel and refuse to send commands")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    args = ap.parse_args()

    if args.list:
        show_ports()
        return

    global _read_only
    _read_only = args.read_only

    port = args.port or find_port()
    if not port:
        print("Could not auto-detect a port. Use -p, or --list to see options.\n")
        show_ports()
        sys.exit(1)

    threading.Thread(target=reader, args=(port, args.baud), daemon=True).start()

    url = f"http://127.0.0.1:{args.port_http}/"
    print(f"Serial : {port} @ {args.baud}")
    print(f"GUI    : {url}   (Ctrl+C to stop)")
    print(f"PWM    : {'read-only' if _read_only else 'control enabled'}")
    if not args.no_browser:
        threading.Timer(0.5, webbrowser.open, args=(url,)).start()

    server = ThreadingHTTPServer(("127.0.0.1", args.port_http), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
