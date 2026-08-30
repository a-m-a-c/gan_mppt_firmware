# Thesis discussion points

Insights worth writing up, not a log. An entry earns its place if it changed a
design decision, contradicted an assumption, or is a general lesson rather than
a project detail. Agents: add one when something genuinely non-obvious turns
up; do not add routine progress.

Each entry: what was observed, why it matters, and the evidence.

---

## Rate limiting the actuator is what makes an aggressive PI loop usable

**Observed.** The CV loop could not be tuned fast without tripping OCP at
turn-on. `KP = 0.015` commands `0.015 x 13000 mV = 195` duty units the instant
the loop starts — a step from zero — and the inrush trips the 12 A comparator.
Detuning until turn-on was safe left the loop far too slow to be useful.

**Why it matters.** The controller and the rate limit answer different
questions: *where should the duty be* versus *how fast can this hardware get
there*. No gain setting encodes the second, because it is a property of the
magnetics and the FETs, not of the control objective. Separating them let
`KP`/`KI` rise by more than two orders of magnitude — settling went from ~5 s to
~200 ms — with turn-on becoming an open-loop ramp.

**Evidence.** Bench capture, 12 V in, 25 V out: a single-step duty change of
~150 units runs clean, ~218 trips OCP. With the limiter at 150/step the slew
flag is high for 4 of 2016 samples, all inside the first 2 s, and silent through
every subsequent line disturbance — the limiter is a large-signal guard, not
something propping up the tuning.

**Caveat that generalises.** The same limiter must not engage during normal
regulation. If it does, it is covering for the gains rather than protecting the
hardware, and the distinction is measurable: record when the limiter is active.

## The current loop trips long before the voltage loop notices

**Observed.** Every failure at turn-on was OCP, never OVP, and it happened
within microseconds of a duty step — far faster than the 55 V overvoltage
threshold could be approached.

**Why it matters.** It sets the priority inside the duty gate. The *slew limit*
is the load-bearing half; the dynamic ceiling protects against a steady-state
overshoot there is time to catch. Designs that reach for a voltage ceiling first
are protecting against the slower of the two failures.

## A bench-derived limit does not transfer to the real source

**Observed.** The slew limit was tuned empirically against a 12 V supply into a
light resistive load — roughly 10 A of headroom to the 12 A trip. The large
array at Impp leaves 3.4 A, and the real bus runs up to 2.2x the bench output
voltage, so `dI/dt` for the same duty step is correspondingly larger.

**Why it matters.** The number is about 6x too permissive for field conditions.
A limit expressed in duty units per step is a *proxy* for the quantity that
actually matters, and the proxy's calibration depends on the operating point.
Either scale it by the operating point, or measure the real quantity.

**Follow-on.** This is the strongest argument for inductor current sensing: a
measured current limit is correct at every operating point without anyone
re-deriving it.

## The measurement rate bounds the control rate, not the other way round

**Observed.** Raising `KI` past a point produced ringing that no further tuning
fixed. The loop time constant had fallen to roughly one sample period.

**Why it matters.** A discrete loop wants its closed-loop time constant at
5-10x the sample period. The sample period here is set by the ADC service
interval, not by the control code — sampling the control loop faster than the
measurement updates just re-reads a stale value. The ceiling on loop bandwidth
is a measurement decision made elsewhere in the system.

## Instrumentation that blocks changes what it measures

**Observed.** Printing two values per control step over UART at 115200 cost
~3 ms of blocked main loop per 100 ms — and would have been 30% of every pass
at the 10 ms control rate.

**Why it matters.** The obvious approach — print as you go — is unusable at
control-loop rates. Recording to RAM (a tick, a name pointer, a float) and
flushing once the run is over removes the observer from the measurement
entirely, at the cost of a bounded buffer and a delay before the data appears.
For a 10 s run at 100 Hz on three series that is 72 KB of RAM and a 12 s flush.

## Regulating the input inverts the loop sign, and nothing announces it

**Observed.** `mode_single_ch_cv.c` regulates the output: more duty raises
`vbus`, so a PI fed `error = setpoint - measurement` with positive gains
converges. `mode_single_ch_mppt.c` regulates the *input*, and the same wiring
diverges — a boost draws harder as duty rises, so `vin` falls as duty rises.
The plant gain from the actuator to the measurement is negative.

**Why it matters.** The failure is not a bad tune, it is a sign error, and it
does not present as one. Positive feedback drives the duty to whichever rail
the first sample points at and holds it there, which looks like a saturated
integrator or a bad clamp. Nothing in `pi_t` records which sign of plant it was
written for, so the assumption lives only at the call site. Any loop that
regulates a quantity *upstream* of the actuator inherits this - the
multi-channel MPPT mode will hit it too, and per-channel current regulation
would as well.

**Evidence.** The 2026-08-29 I-V sweep capture, channel A into the 2.5 ohm
fixture: duty walked 0 -> 700 while `vin` fell 5.515 V -> 2.190 V, monotonic
across the sweep. That is the plant gain, and its sign is unambiguous:
about -4.7 mV of `vin` per duty unit.

**How it is handled.** `mode_single_ch_mppt.c` swaps the pair at the one
`pi_update()` call, which negates the error and leaves `KP`/`KI` positive and
directly comparable with the CV mode's. The alternative - negative gains -
hides the inversion in constants that then cannot be compared against any other
loop in the project.

## The plant gain varies 10x along a PV curve, and a zero-error start cannot move

**Observed.** `mode_single_ch_mppt` appeared not to start from a cold standby,
yet ran normally if an I-V sweep had been run and stopped first. Nothing in the
command path or the init guards differed between the two cases.

Two causes, and they compound:

- `begin()` seeded the P&O target to the *measured* vin, so the initial error
  was exactly zero. The PI had nothing to correct and commanded duty 0, leaving
  the 100 mV P&O steps as the only thing able to get the converter moving.
- Those steps had to act on the flattest part of the curve, where the duty
  barely moves the voltage at all.

The sweep "fixed" it by accident: the seed is taken from a telemetry sample up
to 40 ms old, i.e. from before `pwm_start()` zeroes the duty. Stopping a sweep
at duty 400 leaves that sample at 5.13 V; duty then drops to 0 and vin springs
back to 5.51 V, handing the loop a 380 mV head start it was never designed to
need.

**Why it matters.** The gain from the actuator to the controlled variable is
not a constant of the converter - it is a property of *where on the source
curve you are standing*, and it moves by more than an order of magnitude:

| duty span | dvin/dduty |
|---|---|
| 0 - 100 | 0.96 mV/duty |
| 100 - 200 | 0.55 mV/duty |
| 300 - 400 | 1.4 mV/duty |
| 400 - 500 | 3.1 mV/duty |
| 475 - 575 (at MPP) | 9.85 mV/duty |

A single fixed-gain PI is therefore tuned for one region of the curve and is
wrong everywhere else. Tuned at the knee it is sluggish to the point of looking
broken in the flat region: the loop time constant there is
`1 / (KI * 0.96) = 2.6 s`, so a 100 mV step needs 3.1 s to settle against a
`PO_STALL_MS` of 1 s - every step becomes a timeout step and the setpoint runs
away from the measurement, which is the very failure the arrival gate exists to
prevent.

**The general lesson.** Any controller that starts by matching its setpoint to
the present measurement starts with zero error, and a zero-error start only
works if something *else* moves the operating point. Here the outer hill climb
was that something, and it was too weak to do it. Seeding a deliberate error -
`0.8 x` the present open-circuit voltage - starts the loop with the error
pointing the way the climb is about to walk.

**Evidence.** 2026-08-29 sweep, channel A into the 2.5 ohm fixture: MPP 4.648 V
at duty 525, vin 5.510 V at duty 0, ratio 0.844. That is high for silicon
(0.76-0.80 is the usual fractional-Voc figure) because vin at duty 0 is not a
true open circuit - hardware.md safety note 1's body-diode path is already
feeding ~1.3 A into the fixture, which is visible in the capture as the idle
current at duty 0.
