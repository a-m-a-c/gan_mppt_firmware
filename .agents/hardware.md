# Hardware

Board facts. Everything here is measurable or comes from a datasheet — no
intent, no architecture. If a number here disagrees with the code, one of the
two is a bug; say so rather than silently trusting either.

Pins are configured in CubeMX. **Do not configure pins in code** — if a pin
setting needs to change, say what to change in CubeMX and stop.

---

## MCU and clocks

| Item | Value |
|---|---|
| Part | STM32H743ZIT6 |
| External oscillator | 8 MHz crystal on the RCC HSE pins |
| SYSCLK / CPU (`c_ck`) | **480 MHz** |
| HCLK (AHB) | 120 MHz (`HPRE` = DIV4) |
| HRTIM kernel clock | **480 MHz**, from `c_ck`, prescaler DIV1 → **2.0833 ns/tick** |
| ADC kernel clock | 76 MHz, from PLL2P |

Verified in `SystemClock_Config()` and the `.ioc`, 2026-08-16:
HSE 8 MHz → PLLM 1 → PLLN 120 → VCO 960 MHz → PLLP 2 → **480 MHz SYSCLK**,
VOS0, `FLASH_LATENCY_1` (correct for a 120 MHz AXI clock).
`RCC.HRTIMCLockSelection = RCC_HRTIM1CLK_CPUCLK` — the HRTIM takes the CPU
clock, not the APB2 timer clock.

`PWM_KERNEL_CLOCK_HZ` in `Src/drivers/pwm.c` matches. Every period and
dead-time conversion derives from it, so it must be revisited if the PLL chain
changes.

ADC kernel clock is PLL2: 8 MHz → PLL2N 19 → VCO 152 MHz → PLL2P 2 → 76 MHz,
and the ADCs are initialised `ADC_CLOCK_ASYNC_DIV1`, so f_ADC is the full
76 MHz.

> **⚠ 76 MHz is out of spec. TODO — fix in CubeMX.** The HAL's own BOOST
> brackets (`ADC_ConfigureBoostMode`) top out at `freq/2 > 25 MHz` on Rev.V
> silicon — a **50 MHz** ceiling — and lower on Rev.Y, whose only bracket is
> `> 20 MHz`. At 76 MHz the BOOST field is already pegged and cannot compensate
> further. Conversions still complete, so nothing fails; the numbers are just
> wrong.
>
> Fix, all three ADCs: *Parameter Settings → Clock Prescaler → "Asynchronous
> clock mode divided by 4"*, giving 19 MHz — inside spec for either silicon
> revision. DIV2 (38 MHz) is inside Rev.V's limit but not Rev.Y's, so DIV4 is
> the safe choice while the revision is unconfirmed. The slow group does not
> care: a sweep grows from ~27 µs to ~105 µs against a 10 ms cadence.

---

## Power stage

Five independent **synchronous boost** converters, one per channel (A–E),
driven by HRTIM complementary output pairs.

### Sources

**One array per channel, each a single string.** Two array variants are used;
the firmware must work with both. **Voc cannot exceed the figures below** — no
cold-temperature headroom needs allowing for.

| Array | Voc | Vmpp | Isc | Impp | Pmpp |
|---|---|---|---|---|---|
| Large | 30.5 V | 27.59 V | 8.66 A | 8.22 A | ~226.8 W |
| Small | 13.39 V | 12.11 V | 8.66 A | 8.22 A | ~99.6 W |

### Inductor — 33 µH per channel

Ripple is `ΔI_pp = Vin·D / (L·f)`, worst at the largest `Vin·D` product, which
is the large array into a full bus (27.59 V × 0.495 = 13.66).

| Frequency | ΔI p-p | Peak at Impp (8.22 A) | Margin to 12 A OCP |
|---|---|---|---|
| 500 kHz (default) | 0.83 A | 8.63 A | 28 % |
| 300 kHz | 1.38 A | 8.91 A | 26 % |
| 200 kHz | 2.07 A | 9.25 A | 23 % |
| **100 kHz (config min)** | **4.14 A** | **10.29 A** | **14 %** |
| ~55 kHz | 7.56 A | 12.0 A | trips on ripple alone |

**`PWM_MIN_FREQUENCY_HZ` (100 kHz) is uncomfortably low.** There, normal
operation at the maximum power point already sits at 86 % of the OCP threshold
on ripple alone — a modest transient trips it, and it would present as a
mystery fault rather than as an overcurrent. Ripple also pushes the DCM
boundary up: at 100 kHz the converter drops out of continuous conduction below
2.07 A of input current (25 % of Impp, i.e. ordinary low light), where
`D = 1 − Vin/Vout` stops holding. Raising the floor to 200 kHz would cost
nothing currently in use.

### Battery bus

| | Voltage |
|---|---|
| Vmin | 32.5 V |
| Vnom | 46.8 V |
| Vmax | 54.6 V |

---

## Duty cycle limits (derived)

For a boost converter in continuous conduction:

```
Vout / Vin = 1 / (1 - D)        =>        D = 1 - Vin / Vout
```

Duty is carried in **tenths of a percent** throughout the firmware
(`PWM_DUTY_SCALE = 1000`, so 850 = 85.0 %).

### Required duty by operating point

Highest duty is demanded by the **lowest input** into the **highest bus**.
"Hot panel" assumes a −0.35 %/°C Vmp coefficient at a 65 °C cell (+40 °C over
STC) → −14 % on Vmpp.

| Condition | Vin | Vbus | D |
|---|---|---|---|
| **Large array** | | | |
| Voc into minimum bus | 30.5 V | 32.5 V | 6.2 % |
| Vmpp into nominal bus | 27.59 V | 46.8 V | 41.0 % |
| Vmpp into maximum bus | 27.59 V | 54.6 V | 49.5 % |
| Hot panel into maximum bus | 23.73 V | 54.6 V | 56.5 % |
| **Small array** | | | |
| Voc into minimum bus | 13.39 V | 32.5 V | 58.8 % |
| Vmpp into nominal bus | 12.11 V | 46.8 V | 74.1 % |
| Vmpp into maximum bus | 12.11 V | 54.6 V | 77.8 % |
| **Hot panel into maximum bus** | **10.41 V** | **54.6 V** | **80.9 %** |

### The two limits that follow

**Static ceiling — `PWM_MAX_DUTY_CYCLE = 850` (85.0 %), implemented.**
The worst real operating point is the hot small array into a full battery at
80.9 %. 85 % leaves ~4 points of margin and nothing above it corresponds to a
valid operating point. Boost gain there is 1/(1−0.85) = 6.7×.

The small array is what forces this number up. On the large array alone a 65 %
ceiling would cover everything, so **the static ceiling is a crude backstop,
not real protection** — it sits far above what the large array should ever be
asked for. `pwm.c` rejects out-of-range duty in `pwm_set_duty_cycle()` and
clamps again on the way to the compare register.

**Dynamic ceiling — the one that actually protects the stage. TODO, not
implemented.** The meaningful limit is relative to the instantaneous operating
point:

```
D_max = 1 - Vin / Vbus + margin
```

with Vin from the channel's input INA228 and Vbus from the bus ADC. This is
what stops a duty being commanded that has no valid operating point on the
array currently connected. Margin is TBD. It belongs in the duty gate
(`Src/app/control/control.c`); task 006 in [todo.md](todo.md).

### Frequency / duty / dead-time interaction

Dead time is subtracted from both edges, so it must fit inside the shorter of
the on and off intervals:

```
DT_rising + DT_falling  <  min(D, 1-D) x (1/f)
```

At the current config limits this is violable: 800 kHz (1.25 µs period) at 85 %
duty leaves a 187.5 ns off-time, shorter than the 300 ns maximum dead time.
Even the 500 kHz default at 85 % leaves exactly 300 ns. Any two of the three
parameters constrain the third.

**The driver does not check this combination, deliberately** (2026-08-15). Each
parameter is individually in range; only the combination is invalid, and
reaching it takes a bench operator deliberately setting all three near their
limits. Revisit if anything other than a human starts setting frequency and
dead time.

---

## Protection

### Overcurrent (OCP) — per channel, hardware enforced

An analog comparator per channel asserts a FLT pin when inductor current
exceeds **12 A**. These feed the HRTIM fault inputs, so the outputs are forced
inactive **in hardware** (`FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE`)
before software sees anything. Software latches the fault and keeps the channel
down.

| Channel | HRTIM timer | Fault input | GPIO (live level read) |
|---|---|---|---|
| A | TIMER_A | FLT1 | PA15 |
| B | TIMER_B | FLT2 | PC11 |
| C | TIMER_C | FLT3 | PD4 |
| D | TIMER_D | FLT4 | PB3 |
| E | TIMER_E | FLT5 | PG10 |

**Isc (8.66 A) is below the 12 A OCP threshold.** The array physically cannot
produce a steady-state overload, so *every* OCP trip observed is a transient or
a reverse-current event, not an overload — which is why a ramp rate parameter
exists at all. It limits `dI/dt` during duty steps, not steady current.

**That argument is about the array, and stops holding on a bench supply.** A
supply is a stiff voltage source and will deliver its full current limit into
whatever appears, so an OCP trip on the bench is not automatically a transient.

#### Measured 2026-08-23: a duty step trips OCP, and OVP never gets a look in

Every turn-on failure was OCP, within microseconds of a duty step — the 55 V
overvoltage threshold was never approached. **The slew limit, not the ceiling,
is the load-bearing half of the duty gate.**

Bracket, at **12 V in, 25 V out, light resistive load** (bench supply, roughly
2 A input, so ~10 A of headroom to the trip):

| Single-step duty change | Result |
|---|---|
| ~150 units (15.0 %) | clean |
| ~218 units (21.8 %) | trips OCP |

Both figures were taken at a **10 ms** control step, so the clean rate is
15000 duty/s. `DUTY_SLEW_PER_STEP` in `mode_single_ch_cv.c` is expressed per
step, not per second, so it has to be rescaled whenever `SAMPLE_TIME_MS`
changes. It went to 15/step on 2026-08-24 when the loop moved to 1 ms, which
is the same 15000 duty/s. The per-second rate is the measured quantity; the
per-step constant is derived from it.

**This number does not transfer to the array.** Two things move against it:

- **Headroom.** The large array at Impp sits at 8.63 A peak with ripple —
  3.4 A to the trip, against ~10 A on the bench.
- **Voltage.** A duty step drives inductor current at `dI/dt ≈ ΔD × Vout / L`.
  The real bus runs 32.5–54.6 V against the bench's 25 V, so up to 2.2× the
  slope for the same step.

Stacked, a bench-tuned constant is roughly **6× too permissive** in the field.
Either scale the limit by operating point, or measure current and limit that —
see [thesis_discussion_points.md](thesis_discussion_points.md).

Note the bench supply at 12 V is close to the **small** array's Vmpp
(12.11 V), so it is representative of that case and of nothing else.

### Overvoltage (OVP) — global, software latched

A comparator asserts when the bus exceeds **55 V**, wired to **PC10 /
EXTI15_10**. This is a GPIO interrupt, not an HRTIM fault input: OCP kills the
outputs in hardware, OVP only when the EXTI handler runs.

**Deliberate — the goal was a latch.** Two things make it the right call:

- **HRTIM external events (EEV) do not latch.** They act on the current cycle,
  which makes them the right tool for cycle-by-cycle current limiting and the
  wrong one for an overvoltage that must stay down until something explicitly
  clears it.
- **There is no spare fault input.** The H743 HRTIM has exactly five
  (`HRTIM_FAULT_1`..`_5`) and all five are consumed by the per-channel OCP.
  Fault inputs *do* latch — that is how OCP works — but there is no sixth.

Accepted cost: if interrupts are masked, or a higher-priority handler is
running, the stage keeps switching into an overvoltage bus for that window. If
hardware enforcement is ever wanted alongside the latch it needs the OVP signal
ORed into the existing fault lines **in hardware**, not another CubeMX pin. Not
planned.

Both faults latch and must be explicitly cleared, and a clear is refused while
the physical condition is still present.

---

## Safety notes

Conclusions, not observations — they follow from the numbers above and drive
real constraints on the firmware.

### 1. A channel must start at zero duty and ramp

The ideal diode blocks battery → converter, so before a channel starts its
output capacitor sits at roughly Vin (charged through the high-side body diode)
and **the output is unloaded** until it charges above the bus and the ideal
diode conducts.

An unloaded boost at fixed duty runs toward `Vin / (1 − D)`. At a 50 % duty
with the large array that is `2 × 30.5 ≈ 61 V` — past Vmax (54.6 V) and past
the 55 V OVP trip. Starting a channel at a fixed mid-duty should therefore trip
OVP on the way up, every time.

The correct sequence is: start at **0 % duty** (output = Vin, pass-through),
ramp duty up until the output current sensor shows the ideal diode conducting,
then hand over to the regulator. `PWM_DEFAULT_DUTY_CYCLE` is 0.

### 2. Zero duty is the safe pass-through state — output routing confirmed

`pwm.c` sets output1 at the period and resets it at CMP1, so output1 is active
for the duty fraction — it is the **low-side / control FET**. At 0 % duty
output1 never conducts and the synchronous FET is on, which is the safe
pass-through state a channel starts from.

**Confirmed on the bench, 2026-08-15.** This matters because the safety of a
0 % default depends on it: had the mapping been inverted, 0 % duty would mean
the low-side FET on 100 % of the time — a continuous inductor short across the
input.

At 0 % duty the compare register is floored to `PWM_MIN_COMPARE_TICKS` (3
ticks, 6.25 ns), which is never longer than the rising dead time —
`PWM_MIN_DEAD_TIME_NS` (5 ns) itself rounds up to 3 ticks at 480 MHz — so
dead-time insertion consumes the pulse entirely.

### 3. A cold large array can exceed the minimum bus voltage

At a −10 °C cell (+0.3 %/°C on Voc, +35 °C below STC) the large array Voc
reaches ~33.7 V, above the 32.5 V minimum bus. The high-side body diode would
then forward-conduct into the bus with no switching at all. Not dangerous, but
**"channel stopped" does not always mean "no current flows"**.

---

## Sensors

### INA228 power monitors — 10 devices, 3 mΩ shunts

Two per channel (input and output), all on **I2C1**. 7-bit addresses:

| Channel | Input | Output |
|---|---|---|
| 1 | `0x40` (100 0000) | `0x41` (100 0001) |
| 2 | `0x42` (100 0010) | `0x43` (100 0011) |
| 3 | `0x44` (100 0100) | `0x45` (100 0101) |
| 4 | `0x46` (100 0110) | `0x47` (100 0111) |
| 5 | `0x48` (100 1000) | `0x49` (100 1001) |

These are the MPPT algorithm's current source. The board has a hardware option
to split the ten devices across two I2C interfaces; it is not used.

**Implemented.** `ina228.c` is the device driver, `channel_telem.c` sequences
one interrupt-driven transfer per loop pass and publishes into
`channel_x.telem`, sweeping every `TELEM_SWEEP_PERIOD_MS` (40 ms, 25 Hz).

### Bus voltage

Resistor divider directly on the battery bus, **before** the ideal diode:
100 kΩ top, 5.23 kΩ bottom, into **PA6 / ADC1_INP3**. Thevenin source
impedance is 100 kΩ ∥ 5.23 kΩ ≈ 4.97 kΩ.

**Implemented.** `analog.c` owns ADC1, polls this one input every
`ANALOG_PERIOD_MS` (10 ms, 100 Hz) at `ADC_SAMPLETIME_64CYCLES_5`, and
publishes `sys.vbus_mv` with the divider undone. A failed conversion stores 0,
which is out of range for every consumer, so a dead ADC degrades to "no
reading" rather than to a plausible wrong one.

### ADC channel map — and the split it forces

Eleven analog signals across three ADCs. **The pin assignments are not free:
most of these signals reach exactly one ADC instance**, which settles how the
work has to be divided. Taken from the `.ioc`, 2026-08-16.

| Signal | Pin | ADC input | Reachable from |
|---|---|---|---|
| V_BUS_DIV | PA6 | `INP3` | ADC1 **or** ADC2 |
| NTC_CH1 | PF12 | `ADC1_INP6` | **ADC1 only** |
| NTC_CH2 | PF11 | `ADC1_INP2` | **ADC1 only** |
| NTC_CH3 | PB1 | `INP5` | ADC1 **or** ADC2 |
| NTC_CH4 | PC4 | `INP4` | ADC1 **or** ADC2 |
| NTC_CH5 | PA7 | `INP7` | ADC1 **or** ADC2 |
| I_IND_1 | PF13 | `ADC2_INP2` | **ADC2 only** |
| I_IND_2 | PF3 | `ADC3_INP5` | **ADC3 only** |
| I_IND_3 | PF5 | `ADC3_INP4` | **ADC3 only** |
| I_IND_4 | PF7 | `ADC3_INP3` | **ADC3 only** |
| I_IND_5 | PF9 | `ADC3_INP2` | **ADC3 only** |

Two consequences:

1. **ADC1 must carry the slow group.** NTC_CH1 and NTC_CH2 reach nothing else,
   and once ADC1 is scanning those it may as well take V_BUS and the remaining
   three NTCs — six channels, all slow, all telemetry. (Today it scans V_BUS
   only.)
2. **The current sensors cannot share one ADC.** Channel 1's is on ADC2 and
   channels 2–5 are on ADC3, so no single sequence samples all five. ADC1/ADC2
   sit in the D2 domain and can pair in multimode; ADC3 is in D3 and is
   standalone. Any synchronised current sampling has to trigger ADC2 and ADC3
   separately.

Sampling time is set by source impedance — ~5 kΩ for both the bus divider and a
warm NTC, rising to ~9.5 kΩ for a cold one. At slow-group rates a much longer
sample costs nothing.

### Fast current sensors — TI INA310A2IDGKR

One per channel across the 3 mΩ shunt, sensing **inductor** current.

| | |
|---|---|
| Gain (`A2` suffix) | 50 V/V |
| Scale with a 3 mΩ shunt | **150 mV/A** |
| Output at Impp (8.22 A) | 1.23 V |
| Output at the 12 A OCP threshold | 1.80 V |
| Full 3.3 V range corresponds to | ~22 A |
| Resolution at 16 bits | ~0.34 mA/LSB |

Resolution is far finer than needed — the limit will be the amplifier's offset
and noise, not the ADC.

**The INA310 carries an integrated comparator, which is almost certainly what
generates the FLT/OCP signal at 12 A.** That would make the analog output and
the per-channel fault line the same amplifier, so the analog reading tells you
exactly how close a channel is to tripping, and the 12 A threshold is set by
the comparator reference rather than by anything separate. *Inferred from the
part number — confirm against the schematic.*

The INA228s, not these, feed MPPT.

> **TODO — nothing reads these.** `iind.c` existed and was removed; the
> converter does not depend on it. Reinstating it needs the CubeMX work below,
> and the facts underneath it were expensive to find, so they are kept here.

**CubeMX, ADC2** — Parameter Settings:

| Setting | Value |
|---|---|
| Rank 1 channel | `Channel 2` (PF13, I_IND_1) |
| External Trigger Conversion Source | **`HRTIM TRG1`** |
| External Trigger Conversion Edge | Rising |
| Sampling Time | 16.5 cycles or more |

DMA Settings → Add → `ADC2`, Mode **Circular**, peripheral and memory data
width **Half Word**. *Conversion Data Management is deliberately not in that
list* — CubeMX does not always offer the DMA options for ADC2, and it does not
matter: `HAL_ADC_Start_DMA()` writes `DMNGT` from
`Init.ConversionDataManagement` on every call, so the driver sets it and
whatever the `.ioc` holds is never read.

**CubeMX, ADC3** — four channels, and **the rank order is load-bearing**
because the driver indexes the DMA buffer by position:

| Rank | Channel | Pin | Net |
|---|---|---|---|
| 1 | `Channel 5` | PF3 | I_IND_2 |
| 2 | `Channel 4` | PF5 | I_IND_3 |
| 3 | `Channel 3` | PF7 | I_IND_4 |
| 4 | `Channel 2` | PF9 | I_IND_5 |

Plus Scan Conversion Mode **Enabled**, Number of Conversions **4**, External
Trigger **`HRTIM TRG1`** rising. DMA Settings → Add → `ADC3` — this is a
**BDMA** channel, not DMA1/DMA2 — Circular, Half Word.

**HRTIM needs no CubeMX change.** The master timer and the ADC trigger are
configured at runtime, because the sample rate follows the switching frequency
and cannot be a static setting.

> **Why HRTIM TRG1 specifically:** only triggers 1 and 3 can reach an ADC on
> this part (`ADC_EXTERNALTRIG_HR1_ADCTRG1` / `_ADCTRG3` are the only HRTIM
> entries in the HAL's trigger list). Both ADCs select the same one, so a single
> event starts both sequences and all five channels are sampled from the same
> instant.

> **DMA buffers must be at fixed addresses.** `.bss` links into DTCMRAM here,
> which **DMA cannot reach on the H7** — a normally-declared buffer would simply
> never update. ADC2 is in D2 (DMA1/DMA2, reaches D1/D2) and ADC3 is in D3
> (**BDMA, reaches only D3 SRAM**), so the two buffers must live in different
> regions: `0x30000000` (RAM_D2) and `0x38000000` (RAM_D3), both otherwise
> completely unused. Linker sections would be tidier, but this project's `.ld`
> is CubeMX-generated and gets overwritten, and a silently dropped section
> reintroduces exactly that failure.
>
> D-cache is not enabled (`main.c` never calls `SCB_EnableDCache`). If it ever
> is, both regions must be marked non-cacheable in `MPU_Config()` or every read
> returns stale data.

**ADC2 must have one owner.** If the slow group ever polls PA7 through ADC2 as
a converter cross-check, it cannot do so while ADC2 is held in circular DMA —
that is a second owner of a converter, the collision this project avoids
everywhere else. A one-shot read during init, before the DMA starts, is fine.

### Temperature — Murata NCU18XH103F6SRB

One 10 kΩ NTC per channel to ground with a pull-up to 3.3 V. Telemetry only.

Each NTC reaches its ADC pin through a **1 kΩ series resistor and 100 nF cap**.
That filter is DC-transparent — no current flows into an analog pin in steady
state — so it does not shift the reading. It helps sampling if anything: the
cap is a charge reservoir for the ADC's sample capacitor, which makes the
source look low-impedance at the sampling instant regardless of the
thermistor's own resistance.

> **TODO — nothing samples or reports temperature.** `analog.c` reads the bus
> only, and the generated `ntc_table.h` was removed with it. The divider is not
> understood (below), so reporting was stripped rather than left reporting a
> number that is smoothly wrong. `tools/gen_ntc_table.py` still regenerates the
> table when it is wanted.

**The fitted divider is not the designed one, and it is not resolved.** The
schematic calls for 10 kΩ / 10 kΩ. Measured 2026-08-16, all five channels
within 1 mV of each other:

| | Value | By |
|---|---|---|
| Divider node | **0.342 V** | meter |
| Same node | 0.356 V | ADC1 — agrees, so the converter is fine |
| Thermistor | 11 kΩ | meter — the correct 10 kΩ part at room temperature |
| Top leg, in circuit | 10 kΩ marked, 24.7 kΩ measured | board |
| 3.3 V rail | 3.300 V | meter — solid |

A correct 10 kΩ / 10 kΩ divider on a 3.3 V rail would put that node at
**1.729 V**. It sits at 0.342 V. With rail, node and thermistor all measured
there is no free variable left but the top leg:

```
thermistor draws   0.342 V / 11 kohm   = 31.1 uA
top leg drops      3.300 - 0.342       = 2.958 V
if the thermistor is the only load     Rtop = 2.958 / 31.1u = 95.1 kohm
```

| Model | Top leg | Extra load on the node |
|---|---|---|
| **A** | **~95 kΩ** | none |
| **B** | 10 kΩ | a hidden ~1292 Ω sink, i.e. the ADC pin drawing 265 µA |

Both reproduce 0.342 V exactly. B requires an analog pin to sink 265 µA against
a <1 µA leakage spec, identically on all five channels. **A is also what the
in-circuit 24.7 kΩ implies** — an in-circuit measurement can only read *lower*
than the true part, so the top leg is ≥ 24.7 kΩ, which excludes 10 kΩ; a ~33 kΩ
path through the MCU in parallel with 95 kΩ reads 24.7 kΩ, and no parallel path
can make 10 kΩ read high.

**Non-invasive test — warm one thermistor and watch the pin.** The models
diverge by 10×, because in B the hidden sink swamps the thermistor:

| Thermistor | A predicts | B predicts |
|---|---|---|
| 22 °C (11.0 kΩ) | 342 mV | 342 mV |
| 30 °C (8.2 kΩ) | **261 mV** | 331 mV |
| 37 °C (6.4 kΩ) | **207 mV** | 320 mV |

Grip one for 30 s and read the pin. A fall of 60–80 mV means A, and the fix is
generating the table with `--pullup 100000`; under ~10 mV means B.

> **Two further things unverified, and both fail the same way — smoothly.**
>
> 1. **Is the bottom part really its nominal value?** A single reading near
>    ambient only pins the ratio at one point. A 2.49 kΩ or 3.0 kΩ part in place
>    of 2.2 kΩ still looks sane at room temperature and diverges as things heat
>    up: 2200 Ω reads 80.0 °C where 2490 Ω reads 75.7 °C and 3000 Ω reads
>    69.3 °C.
> 2. **Is it even the same thermistor family?** The generator scales Murata's
>    NCU18XH103 curve, which assumes the fitted part shares its B constant
>    (B25/50 = 3380 K). A different family has a different curve *shape*, not
>    just a different scale — right at ambient, wrong at temperature.
>
> Resolve both by identifying the part actually fitted and generating from its
> own curve. Until then readings are trustworthy near room temperature and
> progressively less so above it — the wrong way round for a converter, where
> the temperatures that matter are the hot ones.

**No β or Steinhart-Hart maths is needed.** Murata's characteristic table for
this part is checked in at `.agents/NCU18XH103F6SRB.csv`: −40 °C to 150 °C in
1 °C steps, min/typ/max columns. It is a *divider output voltage* table rather
than a resistance table, generated for one specific circuit — a 10 kΩ pull-up
to 3.3 V, which its 25 °C value of 1.65 V (exactly half of 3.3 V) confirms.
`tools/gen_ntc_table.py` therefore backs the thermistor curve out of Murata's
reference divider and recomputes the pin voltage for the board's own, so the
divider is a single `--pullup` flag rather than an assumption buried in the
data. With the default 10 kΩ the round-trip is the identity, byte for byte.

Use the **typ** column, monotonically decreasing, as a lookup table with binary
search and linear interpolation. Resolution is comfortable at both ends:
~9 mV/°C at −40 °C and ~2 mV/°C at 150 °C, against ~50 µV per LSB at 16 bits.

---

## Other peripherals

| | |
|---|---|
| **FDCAN** | Two transceivers. FDCAN1 only, initially. Broadcasts telemetry to the car computer and carries state commands. Message IDs, bit rate and framing **TBD**. Not implemented. |
| **Serial** | UART5, 115200 baud, the primary debug interface. Telemetry out, commands in. **TODO — not implemented**; see [workflow.md](workflow.md). |
| **I2C** | I2C1 carries all ten INA228s, interrupt-driven. I2C2 unused. |
| **INJECT_EN** | PD5. Bypasses the ideal diode so current can be injected back into the panels for EL imaging. **Hold low; not used yet.** |

### LEDs

Per-channel toggle LEDs are **active low** (GPIO low = green/on, high =
red/off). System status LEDs are **active high**.

| LED | Pin | Lights when |
|---|---|---|
| LED_TOG_1 | PA4 | channel A PWM running |
| LED_TOG_2 | PA5 | channel B PWM running |
| LED_TOG_3 | PC5 | channel C PWM running |
| LED_TOG_4 | PB0 | channel D PWM running |
| LED_TOG_5 | PB2 | channel E PWM running |
| LED_ACTIVE | PD9 | system state is ACTIVE |
| LED_ERR | PD10 | system state is FAULTED |
| LED_OUT_CONN | PD11 | bus above `LED_BUS_ON_MV` (35.0 V), off below `LED_BUS_OFF_MV` (34.0 V) |

Polarity is a hardware fact and is not configurable. `led.c` inverts the
per-channel lines at the `HAL_GPIO_WritePin()` call and passes the status lines
straight through, so no caller ever needs to know. `status.c` decides when each
lights; `led.c` only makes the pin match.

---

## Open questions

Unanswered. Do not guess these — ask.

1. **INA310 bandwidth / settling time** — decides whether synchronous sampling
   inside a switching on-time (~1 µs at 500 kHz and 50 % duty) is possible.
2. **INA310 comparator → FLT?** Confirm against the schematic that the OCP
   signal comes from the current amplifier's own comparator.
3. **`PWM_MIN_FREQUENCY_HZ`** — leave at 100 kHz, where MPP ripple reaches 86 %
   of the OCP threshold, or raise to 200 kHz? (see Inductor above)
4. **What are the fast current sensors for** — telemetry, or a real inner
   control loop? Decides whether the ADC2/ADC3 path gets rebuilt at all.
5. **Which NTC divider model is right, A or B?** The test is above and needs
   nothing but a warm finger.

Closed: output polarity (confirmed 2026-08-15); OVP on EXTI rather than an
HRTIM fault input; HRTIM kernel clock (verified 480 MHz from `c_ck`,
2026-08-16); ADC kernel clock (76 MHz, confirmed out of spec, fix listed
above); NTC and current sensor part numbers; array arrangement; inductor value.
