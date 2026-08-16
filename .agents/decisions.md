# Decision log

Append-only. Newest at the bottom. Never rewrite an entry — if a decision is
reversed, add a new entry that says so and references the old one.

Format:

```
## NNN — <title>  (YYYY-MM-DD)
**Decision:** what was settled.
**Why:** the reasoning, including the numbers it rests on.
**Rejected:** what else was considered, and why not.
**Status:** implemented / partial / not started / superseded by NNN
```

Entries 001–005 were written retrospectively on 2026-08-15 from the code and
its comments. They record what the code already does and why; the dates are
approximate.

---

## 001 — CubeMX stays a thin init layer  (~2026-07-25)
**Decision:** Generated code is init only. All project logic lives in
`Src/drivers/` (hardware-facing) and `Src/app/` (control and high level), behind
`app_setup()` and `app_loop()`.
**Why:** The `.ioc` is still actively edited and regenerated. Anything outside
`USER CODE` markers is silently clobbered — one regeneration deleted `main()`
including its USER CODE blocks.
**Rejected:** An earlier `mppt_*.c/h` file-naming scheme, dropped in favour of
short module names with matching function prefixes (`pwm_init()`).
**Status:** implemented.

## 002 — Explicit per-channel code over arrays and loops  (~2026-07-25)
**Decision:** Five channels named A–E, named explicitly in the public surface.
Internal lookup tables are fine.
**Why:** The channel count is fixed by the board and will never change. Explicit
code is easier to read and to audit on a safety-critical path than an
index-driven loop.
**Status:** implemented.

## 003 — HAL by default, registers only where the HAL gets in the way  (~2026-07-26)
**Decision:** Use HAL calls except in lock-sensitive/ISR-reachable paths and
where precise write ordering is needed. Flag every exception in a comment.
**Why:** `__HAL_LOCK` makes most HAL calls silently no-op when the handle is
busy. A fault-stop that no-ops because the main loop held the lock is
unacceptable, so fault paths write `ODISR`/`MCR`/`OENR` directly.
**Status:** implemented in `pwm.c`.

## 004 — Requests are separated from application  (~2026-08)
**Decision:** Every command source (serial now; FDCAN and MPPT later) calls
`control_request_*()`, which validates and records but never touches a timer.
`control_service()` is the only code that calls `pwm_*()`, once per loop pass.
**Why:** Several sources can command one channel without racing inside the
driver, and a burst of requests for one channel applies as a single batch in one
stop/start bracket — so `set 1 freq` followed by `set 1 duty` glitches a live
output once instead of twice. Rejection stays synchronous so a transport can
answer its host immediately.
**Status:** implemented. Arbitration is last-writer-wins; `src` is recorded for
diagnostics and is the hook for a real policy later.

## 005 — Host values are rejected, not clamped  (~2026-08)
**Decision:** The command parser rejects out-of-range values. The driver's own
clamps remain as the backstop for internal callers.
**Why:** Quietly substituting a different number hides the difference between
"applied" and "nearly applied".
**Status:** implemented.

---

## 006 — `.agents/` is the source of truth, split by lifetime  (2026-08-15)
**Decision:** Project context moves from a single `project_plan.md` into
`README.md` (index), `project_plan.md` (intent + rules + architecture),
`hardware.md` (board facts), `workflow.md` (build/test/conventions) and this
log. Root `CLAUDE.md` and `AGENTS.md` are one-line pointers.
**Why:** Nothing pointed agents at the plan — neither file name is auto-loaded
by any tool, so it was only ever read by accident. The four kinds of content
also have different lifetimes: agent rules almost never change, hardware facts
change when the board revs, architecture changes constantly. Splitting them
stops a stale architecture note contradicting a hardware fact three paragraphs
away.
**Rejected:** Keeping one file and adding a pointer. It fixes the loading
problem but not the staleness problem.
**Status:** implemented.

## 007 — Decision log exists  (2026-08-15)
**Decision:** This file. Append-only, dated, with reasoning and rejected
alternatives.
**Why:** The stated problem was not knowing where the firmware was going. The
*why* behind the existing design lives only in header comments — excellent ones,
but scattered, and impossible to review as a whole. A log is how a shared mental
model survives across weeks and across two different models.
**Status:** implemented.

## 008 — Duty ceiling set from the two array variants  (2026-08-15)
**Decision:** `PWM_MAX_DUTY_CYCLE` 900 → **850** (85.0 %).
**Why:** `D = 1 − Vin/Vbus`. The worst real operating point is a hot small array
(Vmpp 12.11 V, −14 % at a 65 °C cell → 10.41 V) into a full 54.6 V battery,
which needs **80.9 %**. 85 % leaves ~4 points of margin; nothing above it
corresponds to a valid operating point. Full table in
[hardware.md](hardware.md).
**Note:** The small array alone forces this. The large array never needs more
than 56.5 %, so this static ceiling is a crude backstop and not real protection
on the large array.
**Status:** implemented.

## 009 — Dynamic duty ceiling from the live operating point  (2026-08-15)
**Decision:** The real protection is `D_max = 1 − Vin/Vbus + margin`, computed
from the channel's input INA228 and the bus ADC. Static clamp stays in `pwm.c`;
the dynamic clamp goes in `control.c`.
**Why:** A static ceiling wide enough for the small array is meaningless on the
large one. Putting the dynamic clamp in `pwm.c` would couple the driver to
telemetry; putting it in `control.c` keeps the layering clean and `control.c` is
already the single path to `pwm_*()`. The cost is that `pwm.c` is no longer the
*only* safety authority, so this split must stay explicit.
**Status:** **not started.**

## 010 — Channels start at zero duty and ramp  (2026-08-15)
**Decision:** `PWM_MIN_DUTY_CYCLE` 100 → **0**, `PWM_DEFAULT_DUTY_CYCLE` 500 →
**0**. A channel starts in pass-through and ramps up.
**Why:** The ideal diode blocks battery → converter, so a starting channel's
output is *unloaded* until it charges above the bus. An unloaded boost at fixed
duty runs toward `Vin/(1−D)`; at the old 50 % default with the large array that
is ~61 V, past both Vmax (54.6 V) and the 55 V OVP trip. Starting at a fixed
mid-duty should trip OVP on the way up every time.
**Rejected:** Keeping the 50 % default. It existed so that a wrong output
polarity would not immediately destroy the stage — at 50 % either polarity is
survivable. That protection is real but it is the wrong tool: it makes correct
startup impossible in order to hedge a mistake that should be verified once on a
scope instead.
**Consequence — resolved:** 0 % duty is only safe if HRTIM output1 is the
control FET; an inverted mapping would mean the low-side FET conducting 100 % of
the time. **Confirmed correct on the bench 2026-08-15** — routing checked, so
the 50 % hedge is genuinely no longer needed.
**Status:** implemented in `config.h` and `pwm.c`, routing verified.

## 011 — Zero duty is floored to a legal compare value  (2026-08-15)
**Decision:** `duty_to_compare_ticks()` floors the compare register at 3 HRTIM
ticks rather than allowing 0.
**Why:** CMP1 = 0 is not a legal HRTIM compare value, and the resulting
behaviour is undefined — plausibly leaving output1 permanently set, which is the
catastrophic case. 3 ticks (6.25 ns at 480 MHz) is the documented minimum and is
shorter than the 20 ns default dead time, so dead-time insertion should suppress
the pulse entirely and output1 never conducts.
**Status:** implemented, and the output routing it depends on was verified on
the bench 2026-08-15 (see 010).

## 012 — Ramp rate  (2026-08-15)
**Decision:** Duty changes must be rate-limited. Not yet designed.
**Why:** Bench testing showed sharp duty changes trip OCP. Isc (8.66 A) is below
the 12 A OCP threshold, so the array *cannot* produce a steady-state overload —
every trip is a transient or reverse-current event. The ramp is therefore
limiting `dI/dt` during duty steps, not limiting steady current.
**Status:** not started. Open: where it lives (`pwm.c` vs `control.c`), whether
it is per-channel, and what rate.

## 013 — OVP stays on EXTI, not an HRTIM fault input  (2026-08-15)
**Decision:** The 55 V OVP comparator stays wired to PC10 / EXTI15_10 and is
latched in software. It is not routed into the HRTIM fault system.
**Why:** The requirement is a **latch** — an overvoltage must keep the stage
down until something explicitly clears it, not merely truncate the present
cycle. Two things settle it:
- **HRTIM external events (EEV) do not latch.** They act cycle-by-cycle, which
  makes them right for current limiting and wrong for this.
- **There is no spare fault input.** The H743 HRTIM has exactly five
  (`HRTIM_FAULT_1`..`_5`) and all five are already consumed by the per-channel
  OCP. Fault inputs *do* latch — that is exactly how the OCP path works — but
  there is no sixth one available for OVP.
**Accepted cost:** OCP kills the outputs in hardware; OVP only when the EXTI
handler runs. With interrupts masked or a higher-priority handler running, the
stage keeps switching into an overvoltage bus for that window.
**Rejected:** Routing OVP to a fault input — impossible without giving up a
channel's OCP. Getting hardware enforcement *and* the latch would need the OVP
signal ORed into the existing fault lines in hardware. Not planned.
**Status:** implemented (this is what the code already does); now recorded as
deliberate rather than incidental.

## 014 — Frequency / duty / dead-time combination left unchecked  (2026-08-15)
**Decision:** The driver does not validate that dead time fits inside the
shorter of the on and off intervals. Left as-is for now.
**Why:** Each parameter is individually range-checked; only the combination can
be invalid (800 kHz at 85 % duty leaves a 187.5 ns off-time against a 300 ns
maximum dead time). Reaching it takes a bench operator deliberately pushing all
three toward their limits, and the bench is currently the only thing setting
frequency and dead time.
**Revisit when:** anything other than a human sets frequency or dead time — an
autotuning step, a CAN command, or the MPPT loop.
**Status:** accepted gap, documented in [hardware.md](hardware.md).

## 015 — `analog.c` is the single owner of ADC1; `vbus.c` is gone  (2026-08-16)
**Decision:** One module owns ADC1 and sweeps all six slow inputs — V_BUS_DIV
and the five NTCs — polled, one sweep every `ANALOG_PERIOD_MS`. `vbus.c` and
`vbus.h` are deleted, their bus-voltage code absorbed; `vbus_millivolts()`
becomes `analog_vbus_mv()`.
**Why:** The pin map leaves no choice about *which* ADC (NTC_CH1 and NTC_CH2
reach ADC1 alone), and two modules configuring rank 1 on the same instance
would race. `vbus.c` had already flagged this — "Revisit when the NTC channels
need sampling too".
**Rejected:**
- *A separate `ntc.c` beside `vbus.c`.* Less churn, but two owners of one
  peripheral is the collision the project avoids everywhere else.
- *Scan mode + DMA.* The natural shape for a fast group, but overkill at 100 Hz:
  one sweep is ~27 µs, and DMA would add a generated-code change and a coherency
  question for a saving of 0.3 % loop time.
**Status:** implemented, builds clean. **Not verified on hardware.**

## 016 — Fast current sensing deferred, not designed  (2026-08-16)
**Decision:** ADC2 and ADC3 stay untouched. When built, they will be triggered
from an HRTIM pulse; ~100 kHz per channel is more than enough.
**Why:** The sensors exist for a control loop and nothing else — not telemetry
(the INA228s are better) and not overcurrent (the INA310's own comparator does
that in hardware). Their design depends on which control algorithm is chosen,
which is undecided, so building now means guessing the trigger point and
rebuilding later.
**Note for whoever builds it:** the trigger matters more than the rate. Inductor
current is a triangle — 0.83 A p-p on 8.22 A at 500 kHz — so sampling at an
arbitrary phase gives a random point on the ripple, while sampling at the
midpoint of the on-time gives the average with no filter. Also, channel 1's
sensor is on ADC2 and channels 2–5 are on ADC3, so no single sequence covers
all five; round-robin on ADC3 samples each at its own correct instant and still
gives 125 kHz per channel.
**Status:** not started, by decision.

## 017 — NTC conversion is a generated lookup table  (2026-08-16)
**Decision:** `tools/gen_ntc_table.py` turns Murata's CSV into
`Inc/drivers/ntc_table.h` — 191 entries, −40 to 150 °C, stored as tenths of a
millivolt. `analog.c` does binary search plus linear interpolation. Both the
script and the generated header are committed.
**Why:** The CSV is already a *divider output voltage* table generated for a
10 kΩ pull-up to 3.3 V — its 25 °C value is 1.65 V, exactly half of 3.3 V,
which is the check that it matches the board. So no β or Steinhart-Hart maths,
no floating point, and no runtime cost beyond 382 bytes of flash and ~8
comparisons.
**Why tenths of a millivolt** rather than whole millivolts: at the hot end the
curve flattens to ~2 mV/°C, where whole millivolts would quantise the result to
about half a degree.
**Two rounding bugs found and fixed by testing the arithmetic against the source
CSV**, both at the hot end where the curve is flattest:
- Truncating in `raw_to_dmv()` biased every reading half a count low, enough to
  push a reading landing exactly on the last table entry off the end of the
  table — reporting a fault for a working sensor. Now rounds.
- Table entries and converted readings are the same curve rounded
  *independently*, so at the extremes they can still land one count apart (the
  150 °C entry stores 988; the ADC path reconstructs 987). A one-count slack
  band at each endpoint, pinned to the endpoint, closes it.
Verified end to end against the CSV: exact table entries are lossless, and full
round-trip through 16-bit ADC quantisation is within **0.10 °C** across
−40..150 °C, with open and short both detected.
**Status:** implemented and arithmetically verified. Bench run 2026-08-16 showed
the conversion working but the assumed divider wrong — see 018.

## 018 — The NTC table is generated for a divider, not from one  (2026-08-16)
**Trigger:** First bench run read 95–96 °C on all five channels of a
room-temperature board. Five channels agreeing rules out sensor faults; the
error is systematic.
**Diagnosis:** A 10× divider-ratio error. Backing R(T) out of Murata's curve
and re-deriving: a 100 kΩ pull-up where the table assumes 10 kΩ reads a 22 °C
board as 94.7 °C and a 25 °C board as 99.0 °C. A 1 kΩ thermistor under a 10 kΩ
pull-up gives identical voltages, so the two cannot be told apart electrically
— only by a meter on the parts. **Not yet confirmed which.**
**Decision:** `gen_ntc_table.py` now backs the thermistor curve out of Murata's
reference divider (10 kΩ / 3.3 V, which its 1.65 V at 25 °C confirms) and
recomputes the pin voltage for the board's actual divider, taken from
`--pullup` / `--rail`. Default 10 kΩ round-trips to a byte-identical table.
**Why:** The CSV silently encodes a circuit. Consuming its voltages directly
made that assumption invisible, and an invisible assumption about a divider
fails *smoothly* — it produces a plausible number rather than an obvious fault,
which is exactly why this cost a bench session. The generated header now names
the divider it was built for in its own header comment.
**Also added:** an `adc` host command reporting raw pin millivolts with nothing
applied — no divider undone, no table consulted — and `stream_telem.py --adc`
to poll it. That measurement separates an ADC problem from a wrong assumption
about the circuit feeding it, which is the split no amount of staring at
converted temperatures can resolve.
**Outcome:** the divider hypothesis was **wrong** — the resistors were confirmed
as 10 kΩ into 10 kΩ. The generator change stands on its own merits (the
assumption is now visible and parameterised) but it did not explain the fault.
See 019.
**Status:** implemented; superseded as a diagnosis by 019.

## 019 — The ADC is clocked out of spec at 76 MHz  (2026-08-16)
**Finding:** `adc_ker_ck` is 76 MHz from PLL2 and all three ADCs use
`ADC_CLOCK_ASYNC_DIV1`, so f_ADC is the full 76 MHz. The HAL's own BOOST
brackets (`ADC_ConfigureBoostMode`) top out at `freq/2 > 25 MHz` on Rev.V — a
**50 MHz** ceiling — and Rev.Y's only bracket is `> 20 MHz`. BOOST is already
pegged at maximum and cannot compensate.
**Why it matters here:** with the divider confirmed correct, the pin voltage
must be right, so the ADC must be reading low — and an over-clocked SAR reads
low, because each bit trial gets too little time for the comparator to settle
and the decision falls the wrong way. All five channels being wrong by the same
factor fits a converter fault, not five circuit faults.
**Fix:** CubeMX, all three ADCs, *Clock Prescaler → divided by 4* (19 MHz),
inside spec for either silicon revision. The slow group is indifferent: a sweep
grows from ~27 µs to ~105 µs against a 10 ms cadence.
**Added to settle it without a meter:** `analog_measure_vrefint()` reads the
internal reference on ADC3 once at startup and the `adc` command reports it
against `VREFINT_CAL`, the raw count ST measured in the factory at VDDA =
3.3 V. Raw compared to raw, no arithmetic in between. VREFINT has no pin, no
divider and no sensor behind it, so it isolates the converter from everything
else on the board — the measurement that would have skipped the whole divider
detour had it existed first.
**Lesson:** this was flagged as open question 1 on 2026-08-16 and left
unresolved while two sessions went into the sensor circuit. A known out-of-spec
peripheral clock should have been ruled out before any hypothesis about the
parts hanging off it.
**Status:** prescaler changed to DIV4 and the ADC is now in spec — but this was
**not** the cause of the temperature error. VREFINT came back at ratio 0.998,
so the ADC was converting correctly all along. Keep the DIV4 setting regardless:
running a peripheral 52 % over its rated clock is a defect whether or not it
was causing this particular symptom.

## 020 — The temperature fault is a 10× divider ratio, measured  (2026-08-16)
**Measurement** (`stream_telem.py --temps --adc`): all five NTC pins at
**356 mV**; VREFINT measured 24248 against factory 24304, **ratio 0.998**.
**Conclusion:** the ADC is correct and the firmware's arithmetic is correct —
91.7 °C is the right answer for 356 mV under a 10 kΩ / 10 kΩ divider. The pins
genuinely sit at 356 mV, so the board's divider ratio is ~9.3:1 where the table
assumes 1:1. Bottom leg computes to 1211 Ω against the ~11–12 kΩ a 10 kΩ NTC
should show at room temperature.
**Two candidates, indistinguishable at the pin** because only the ratio reaches
the ADC — a 1 kΩ thermistor under a 10 kΩ pull-up, or a 10 kΩ thermistor under
a ~100 kΩ pull-up. Both predict 0.356 V at 20 °C against 0.3565 V measured.
Resolving it needs a meter on a part, not more firmware.
**Generator gained `--ntc25`** so the fitted thermistor can be named directly
rather than expressed as an equivalent pull-up. `--ntc25 1000` and
`--pullup 100000` produce byte-identical tables (verified), which is the same
fact stated twice; the flag should match what is actually on the board so the
generated header's provenance comment does not lie.
**What actually found it:** the VREFINT self-check from 019. Three hypotheses
had been argued from symptoms — wrong pull-up, then a wrong ADC clock — and
both were wrong. One internal reference reading, with no pin or divider behind
it, split "the ADC is lying" from "the circuit is not what we think" in a
single line. **Measure the thing that isolates the subsystem before theorising
about either half.**
**Status:** resolved — see 021.

## 021 — Temperature reads correctly; the board is not the schematic  (2026-08-16)
**Root cause:** JLC populated the wrong parts on the NTC divider. Designed
10 kΩ / 10 kΩ; fitted is a **24.7 kΩ top leg** with a bottom leg measuring
~2992 Ω at room temperature, against the ~11–12 kΩ a 10 kΩ NTC should show.
Two wrong parts on one net, which is why the first correction did not land: a
24.7 kΩ pull-up alone still predicts 1.08 V at 20 °C against the 0.3565 V
measured.
**Fix:** `uv run tools/gen_ntc_table.py --pullup 24700 --ntc25 2200`, giving
16.9 °C at 356 mV — ambient on the bench. No C changed; the table is the
conversion.
**Vindicated the design of 018:** because the divider is a generator parameter
rather than an assumption baked into the data, a board that differs from its
own schematic cost one flag and a reflash instead of a code change.
**Left open, deliberately:** whether the bottom part is really 2200 Ω nominal,
and whether it shares the NCU18XH103's B constant. A single point near ambient
fixes the ratio there and says nothing about the curve's shape — a 3.0 kΩ part
would read 10.2 °C at a true 18 °C and 69.3 °C at a true 80 °C, plausible at
both. Identifying the fitted part and generating from its own curve is the only
real close-out. Recorded in [hardware.md](hardware.md).
**Lesson, and it is the same one three times over:** every wrong answer in this
hunt was *plausible*. 91.7 °C, then 60 °C, now 17 °C — each looked like a
reading rather than a fault, and only arithmetic against an independent
measurement told them apart. A sensor chain that cannot be checked against
something external is not verified merely because its output looks reasonable.
**Superseded in part by 022** — the 24.7 kΩ reading is itself now in doubt.

## 022 — The 24.7 kΩ reading is probably a measurement artifact  (2026-08-16)
**Doubt raised:** the top-leg package on the board looks correct, and the same
part value reads correctly elsewhere on more isolated circuits.
**Supporting physics:** an in-circuit resistance measurement **cannot read
high**. Anything in parallel with the part under test can only pull the reading
*down*, so 24.7 kΩ across a 10 kΩ resistor is not explainable by the
surrounding network — it points to bad probe contact, the wrong two points, or
a series path through the meter, not to a wrong part.
**Only the ratio is established:** `Rtop/Rbottom = (3.300 − 0.356)/0.356 =
8.26`. Absolute values need one independent measurement, which has not been
made.
**Candidate A — 10 kΩ top (correct) with a 1 kΩ NTC, one wrong part.** Predicted
0.356 V at 20 °C *before* any resistor was measured, against 0.3565 V observed —
agreement to half a millivolt. Implies a 19.9 °C room. A `102`/`103` reel
mix-up is the classic version of this error.
**Candidate B — 24.7 kΩ top with a 2.2 kΩ NTC, two wrong parts on one net.**
Implies a 16.9 °C room. The 2200 Ω was a value *suggested as plausible*, never
measured, then adopted because the result looked right — which is the
confirmation-bias version of the same trap this whole hunt has been about.
**Assessment:** A is more likely on three counts — one wrong part instead of
two, a more ordinary room temperature, and a prediction that landed before the
fact. It is still inference, and the table stays on B until measured, because
switching on inference is exactly how the last three rounds went.
**Cost of getting it wrong:** ~3 °C at ambient, ~4 °C at 80 °C. Tolerable for
telemetry, not for a thermal derate, and the generated header would document a
false cause either way.
**Test:** node-to-ground resistance, board unpowered — see
[hardware.md](hardware.md). ~1.1 kΩ means A, ~2.7 kΩ means B.
**Outcome:** neither. The thermistor measured **11 kΩ** — the correct part. See
023.

## 023 — It was the top leg all along: ~100 kΩ, not 10 kΩ  (2026-08-16)
**Measurement:** thermistor reads 11 kΩ, i.e. the correct 10 kΩ NCU18XH103 at
room temperature. With the ratio fixed at 8.26 by the pin voltage, the top leg
is **~91–100 kΩ** against the 10 kΩ on the schematic. The earlier 24.7 kΩ was
an artifact of measuring through the MCU's ADC path rather than across the
resistor — consistent with 022's argument that an in-circuit reading cannot
legitimately read high.
**Generated for `--pullup 100000`**, reading 356 mV as 19.9 °C.
**Residual ~2.5 °C ambiguity:** if the 11 kΩ was read in circuit, the true
thermistor is 12.08 kΩ (shunted by 100 kΩ reads 10.8 kΩ, rounding to 11 kΩ) and
the top leg is 100 kΩ → 19.9 °C. If read with a leg lifted, the thermistor is
11.0 kΩ and the top leg is 90.9 kΩ → 22.4 °C. Both self-consistent; 100 kΩ
assumed as the far more common part. A room thermometer settles it.
**This was the first hypothesis, on 2026-08-16, abandoned on assurance.** The
original arithmetic said "a 100 kΩ pull-up where the table assumes 10 kΩ reads a
22 °C board as 94.7 °C" — correct, and dropped when the schematic value was
offered as confirmation. Two sessions then went into the ADC and the thermistor.
**Lesson:** the schematic is not the board, and a design intent is not a
measurement. When arithmetic from a live reading contradicts a stated component
value, the reading is the evidence and the stated value is the hypothesis — I
had it the other way round. What finally broke it was measuring one component
in isolation, which is the same lesson as 020 and 022, arrived at a third time.
**Status:** **withdrawn** — the top leg is confirmed 10 kΩ on the board. See 024.

## 024 — Three measurements that cannot all be true  (2026-08-16)
**The contradiction:** top leg 10 kΩ (board), thermistor 11 kΩ (meter), ADC pin
356 mV (ADC1). The first two put that node at 1.73 V. The third says 0.356 V.
A factor of 4.9 apart, and at most two of the three can be right.
**Why no table can settle this:** a lookup table only ever encodes the *ratio*.
Any assumed divider that maps the present pin voltage onto room temperature
will look right today and be wrong at temperature. Every round of this hunt has
produced a plausible number — 91.7, 60, 17, 19.9 °C — and plausibility has been
worthless as evidence each time. The table currently carries `--pullup 100000`
as a **placeholder consistent with the ratio, not a believed description of the
board**, and the generated header should not be read as a claim about hardware.
**Correction to 019/020:** the VREFINT self-check validated **ADC3**, not ADC1.
VREFINT is an ADC3-only channel on the H743, so "the ADC is correct, ratio
0.998" was true of the wrong converter. ADC1 — the one reading the thermistors
— has never been independently checked. That gap is mine; it made a partial
result sound conclusive and helped push the search onto the board.
**Added:** PA7 is wired to `INP7` on both ADC1 and ADC2, so `analog.c` now reads
that one pad through both converters each sweep and the `adc` command reports
the pair. Same pad, two independent ADCs, no external equipment.
- agree → the pad really is at 356 mV; the fault is on the board
- disagree → ADC1 is misconverting and every NTC reading so far is void
**Still missing after five rounds:** nobody has put a **meter on the divider
node with the board powered**. It was suggested early and never taken, and it
answers the whole question in one reading. Prefer it over any further inference.
**Outcome:** the meter read 0.342 V, agreeing with ADC1's 0.356 V. See 025.

## 025 — The excitation rail, not the divider  (2026-08-16)
**The measurement that ended it:** meter on the divider node reads **0.342 V**,
against ADC1's 0.356 V. They agree, so ADC1 was never at fault and the pad
really is at ~0.35 V.
**Resolution:** with both resistors confirmed (10 kΩ top, 11 kΩ thermistor =
the correct part at room temperature) and the node voltage confirmed by two
instruments, the only remaining free variable is the **supply feeding the top
of the divider**:
```
V_rail = V_node x (Rtop + Rntc)/Rntc = 0.342 x 21/11 = 0.653 V
```
A 3.3 V rail would put the node at 1.729 V. **The NTC pull-up rail is at ~0.65 V
— a silicon diode drop, the signature of a floating net back-fed through a
junction rather than driven.** Currents balance as a plain divider: 31 µA
through each leg. All five channels within 1 mV of each other fits one shared
rail, not five faults.
**Explains the original symptom exactly:** correct parts, correct table, node at
0.342 V → 93.4 °C, which is the 91–98 °C from the first bench run.
**Firmware needs no change.** The table is back to the designed 10 kΩ / 10 kΩ
and will be right the moment the rail is. Every table generated in rounds
018–023 was fitting the *ratio* around a wrong excitation voltage — which is
why each produced a plausible number and none were correct.
**Next:** measure the top pad of an NTC pull-up. Expect 3.3 V; ~0.65 V confirms.
Then trace the net — an unpopulated ferrite, 0 Ω link or series part between it
and 3V3 is the usual cause.
**Outcome:** rail measured 3.300 V — solid. Wrong again. See 026.

## 026 — Everything measured; the top leg is the only term left  (2026-08-16)
**All four quantities now measured:** rail 3.300 V, node 0.342 V (meter, ADC1
agrees at 0.356), thermistor 11 kΩ. No free variable remains, and they only
reconcile two ways:

| Model | Top leg | Extra load on the node |
|---|---|---|
| **A** | **95.1 kΩ** | none |
| **B** | 10 kΩ | hidden ~1292 Ω sink — the ADC pin drawing 265 µA |

Both reproduce 0.342 V exactly. B needs an analog pin to sink 265 µA against a
<1 µA leakage spec, identically on five channels.
**A is independently supported** by the in-circuit 24.7 kΩ reading: an
in-circuit measurement can only read *lower* than the true part, so the top leg
is ≥ 24.7 kΩ, excluding 10 kΩ. A ~33 kΩ path through the MCU across a 95 kΩ
resistor reads 24.7 kΩ. That reading was dismissed as an artifact in 022–023
when it was in fact evidence — the artifact explanation the author gave (a
parallel path through the µC) is precisely what drags 95 kΩ down to 24.7 kΩ.
**Non-invasive discriminator, since nothing is to be lifted:** warm one
thermistor. A predicts the pin falls 342 → 261 mV by 30 °C; B predicts
342 → 331 mV, because the hidden sink swamps the thermistor. A 10× difference
in response, using only the `adc` command.
**Method note:** the earlier tests all probed *static* values, which is why each
one could be argued away. Perturbing the system and comparing the *response*
against two models discriminates where a single reading cannot — and it needed
no hardware access at all. Worth reaching for earlier next time.
**Status:** parked unresolved by decision — see 027.

## 027 — Temperature parked: driver kept, application stripped  (2026-08-16)
**Decision:** Stop here. `analog.c` keeps sampling and converting the NTCs, and
the lookup table stays generated for `--pullup 100000`. The **application no
longer reports temperature**: it is out of the telemetry CSV (back to 23
fields), out of `valid_mask` (back to five bits), and out of both host tools.
**Why:** the divider is still not understood after six rounds, temperature is
telemetry-only today, and nothing depends on it. Carrying a number that is
smoothly wrong is worse than carrying none — a plausible reading invites trust,
and this one has produced four different plausible answers.
**What stays, and why it is not "temperature code":** the `adc` command still
reports raw ADC pin voltages, VREFINT against factory, and the ADC1/ADC2
cross-check on PA7. Those are converter diagnostics, not temperature, and they
are the instrument for finishing this later. Removing them would throw away the
only tooling that made progress.
**To resume:** the finger test in 026 discriminates the two surviving models
without touching hardware; then `--pullup <ohms>`, re-add the field to
`append_channel()` and the mask bit, and restore the series in the host tools.
**Blocking condition:** if temperature ever gates a thermal derate or shutdown,
this must be resolved first — a smoothly wrong reading in a protection path is
the failure mode that matters.
**Status:** closed as parked. Not a fix.

## 028 — `iind.c`: inductor current abstraction over ADC2/ADC3  (2026-08-16)
**Decision:** A driver named for the board's nets (`I_IND_1..5`) so it cannot be
confused with the INA228 input/output currents. Channels are addressed by
`pwm_channel_id_t`, so a caller commands and measures the same channel by the
same name — that mapping is the point of the layer. Supersedes 016's deferral.
**Sampling:** the HRTIM **master timer**, unused by `pwm.c`, becomes the sample
clock. Its period is a whole multiple of the switching period so the sample
instant stays locked to the waveform instead of walking through it; master
compare 1 places the instant and raises HRTIM ADC trigger 1, which starts both
ADCs at once.
**Why the trigger and not the rate:** inductor current is a triangle, ~0.83 A
p-p on 8.22 A at 500 kHz with 33 µH. An arbitrary phase returns a random point
on it and oversampling cannot help, because it is not noise. `IIND_SAMPLE_DIVIDER`
= 5 gives 100 kHz — the rate the author called ample — and leaves ADC3 10 µs to
convert four channels rather than the 2 µs one switching period allows.
**Why HRTIM TRG1:** only triggers 1 and 3 can reach an ADC on this part. Both
ADCs select the same one, so all five channels come from one instant.
**Zero calibration** is measured, not assumed — the amplifier may sit at ground
or at a mid-rail reference, and either way carries its own offset.
`iind_calibrate_zero()` **refuses unless every channel is stopped**: calibrating
against a live converter folds the operating current into the offset and biases
every later reading by exactly that amount, which reads as a plausible current
rather than as an error. That invariant is the whole reason the function exists
separately from `iind_init()`.
**DMA buffers at fixed addresses (0x30000000 / 0x38000000).** `.bss` links into
DTCMRAM, which **DMA cannot reach on the H7** — an ordinary buffer would never
update, with no error anywhere. ADC2 is in D2 (DMA1/2) and ADC3 in D3 (**BDMA,
which reaches only D3 SRAM**), so they cannot share a region. Linker sections
would be tidier, but the `.ld` is CubeMX-generated and overwritten, and a
silently dropped section reintroduces exactly that failure.
**Soft-fails throughout.** A current sensor that cannot configure itself must
not take the board down, so nothing here calls `Error_Handler()` — unlike
`pwm.c`, where halting is the right answer.
**Known limitation:** the master timer is not phase-aligned to timers A..E, so
`iind_set_sample_point()` has a constant but unknown offset from a channel's
switching edge and must be calibrated on a scope. Fixing it means changing how
`pwm.c` starts its timers.
**Conversion Data Management is set in code, not CubeMX** (added 2026-08-16
when the ADC2 dropdown turned out not to offer DMA modes). `HAL_ADC_Start_DMA()`
writes `DMNGT` from `Init.ConversionDataManagement` on every call, so the `.ioc`
value is never read. Setting it in the module that depends on it removes a
bring-up step and stops the mode drifting out from under the driver.
**ADC2 gets a single owner.** `analog.c` was reading PA7 through ADC2 every
sweep as a converter cross-check, which would have fought `iind`'s circular DMA
for the same peripheral — the two-owner collision this project avoids
everywhere else, and one that would have appeared as intermittent garbage on
both. That read now happens once in `analog_init()`, before `iind` starts; the
pin voltage is static so nothing is lost.
**Status:** builds clean and is called from `app_setup()`. **Non-functional
until the CubeMX changes in [hardware.md](hardware.md) are made** — ADC2/ADC3
triggers, ranks and DMA requests. `iind_init()` returns false until then,
deliberately and visibly. **Nothing is verified on hardware.**

## 029 — Host surface for inductor sensing  (2026-08-16)
**Decision:** Inductor current joins the telemetry CSV as a fifth field per
channel (28 fields), `valid_mask` gains bits 5..9 for live sampling, and an
`iind` command family drives the sampler: `start`, `stop`, `zero`,
`point <tenths>`, and a bare `iind` for status. The `#iind` status line rides
the same 1 s cadence as the `#cfg` set.
**Why the CSV and not only a command:** the GUI plots from the CSV, so a field
there is what makes the current *trackable* rather than merely readable. At
20 Hz against a 100 kHz sample rate it is a trend, not a waveform — noted in
command.h so nobody mistakes it for one. A control loop reads `iind.h` directly.
**Why a command family at all:** nothing called `iind_start()`, so there was no
way to make the sampler run, and `iind_calibrate_zero()` has to be invokable at
a moment when the channels are stopped. A refused calibration is surfaced as
`#err,running` rather than swallowed — a zero that silently did not happen
leaves every later current wrong by exactly the operating point.
**GUI shows a rate, not a count.** `sample_id` is differenced into samples per
second, and `running` with `0/s` is coloured as a fault. That combination means
the HRTIM trigger is not firing, and it is invisible on every other indicator —
the state is right, the ADCs are armed, the numbers are plausible, and nothing
is arriving. The calibrated zeros are printed alongside because every plotted
current is measured against them.
**Status:** firmware and both host tools build and parse; page structure and
the `#iind` path verified against a live server. **Not verified on hardware** —
and cannot be until the CubeMX changes land.
