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
