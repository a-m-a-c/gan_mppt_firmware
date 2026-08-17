# Plan

Intent, working rules, and where the firmware is going. Board facts live in
[hardware.md](hardware.md); build and style in [workflow.md](workflow.md);
reasoning behind past choices in [decisions.md](decisions.md).

## Introduction (DO NOT EDIT)
This is the firmware for a GaN MPPT device with 5 seperate channels. I am writing this document because I feel like I do not understand the way the firmware is going, nor do you understand what I want this to be.

## Agent Instructions (DO NOT EDIT)
This is an eductional experience, and in fact my first introduction to designing firware from the ground up. At work, I have worked and modified firmwares for other devices such as ESC, but I have never designed it from the ground up. With this being the case, I would like you to push me to learn, rather then just doing everything for me. However, what I would like you to do is design the bare interfaces, such as what we have already done with the PWM hal abstraction, and the INA drivers. 

Simplicity and visibility is the goal here, continue on with the current folder structure. Lean towards simple and verifiable solutions, rather then complex optimisations. Saftey is critical, firmware functions that cause the PWM duty cycle to go high have the potential to damage the board itself, although this is unlikely with the current interrupts.

I would prefer to use cube mx to configure pin settings, do not configure these yourself unless necessary, and instead tell me what to change in cube mx. Additionally, DO NOT edit the cube mx generated code in any way. You are only allowed to write inside the cube functions.

Append additional context to this file, do not edit sections with "DO NOT EDIT" unless we discuss and deem it to be useful.

This file alongisde the codebase is the main source of truth, and anything else in .agents. Consider the fact that I will be using both GPT5.6 SOL and CLAUDE OPUS 5 in writing this, so avoid model specific memory locations, and instead make a new file inside .agents to store information.

## Working Agreement

Added 2026-08-15 by agreement; the sections above are unchanged.

- **State the interface before writing the implementation.** Name the functions,
  the types, and the invariant each one protects, and get agreement before
  producing code. The instruction above is to design bare interfaces and push
  the learning back — without a hard gate, agents drift into writing the whole
  module.
- **Say whether a change was verified or only compiled.** There is no automated
  test suite; verification is manual on hardware. An agent can only get code to
  build. Never report a hardware behaviour as confirmed.
- **Numbers carry their arithmetic.** Any limit, threshold or cadence is written
  down with the calculation that produced it, in the comment next to it and in
  [hardware.md](hardware.md) if it comes from the board.
- **Where new context goes:** board facts → [hardware.md](hardware.md);
  reasoning → [decisions.md](decisions.md); build/style →
  [workflow.md](workflow.md); intent and architecture → this file. Nothing
  project-specific goes into a model's private memory.
- **CubeMX changes are instructions, not actions.** Say what to change in the
  `.ioc` and stop.

## Firmware Architecture

Simplicity and clean, understandable code, with room to experiment with control
algorithms. Configuration lives in `Inc/config.h`; anything that follows from
the hardware and can never change is hardcoded next to the code that uses it.

### What exists today

```
main() ──> app_setup()            one-time bring-up
       └─> app_loop()             one non-blocking pass, forever
             telem_service()      advances one I2C transfer
             vbus_service()       one ADC conversion
             command_service()    parse host lines -> post requests
             control_service()    apply pending setpoints -> pwm_*()
             command_report_service()   emit #cfg + telemetry CSV
             led_service()        pure reads, no I/O
```

The load-bearing rule is that **`control_service()` is the only code that calls
`pwm_*()`.** Every command source — serial now, FDCAN and MPPT later — posts a
request and returns. See [decisions.md](decisions.md) 004.

`pwm.c` is the safety boundary: it owns the clamps, the fault latches, and the
critical sections around output enable/disable.

### The control stack

Designed 2026-08-16 in conversation. Reasoning and rejected alternatives are in
[decisions.md](decisions.md) 030 and 031; this is the shape.

```
  SOURCES        GUI / serial        MPPT algorithm        FDCAN (later)
                      |                    |                    |
                      +----------+---------+--------------------+
                                 v
  INTENT         regulate.c   per-channel mode + state machine + setpoint
                              picks a duty; never touches hardware
                                 |
                                 v
  CONDITIONING   duty gate    slew limit  ->  dynamic ceiling
                              every duty from every source passes here
                                 |
                                 v
  APPLY          control.c    batches pending changes, one stop/start bracket
                              the only caller of pwm_*()
                                 |
                                 v
  DRIVER         pwm.c        static clamps, fault latches, output enable
                                 |
                                 v
  SILICON        HRTIM        OCP forces outputs inactive in hardware

  MEASUREMENT    analog.c (bus V, 100 Hz)
                 channel_telem.c (INA228 pairs, 25 Hz)   -> read by regulate.c
                 iind.c (100 kHz)  telemetry only, for now   and by the gate
```

Two rules hold it together:

- **The gate is unconditional.** Slider, MPPT, CV loop, CAN — every duty is
  slew-limited and ceiling-clamped in one place. There is no path around it.
  This is where [decisions.md](decisions.md) 009 and 012 land.
- **`Vbus_ref` is the voltage the channel is *trying to reach*, never the
  voltage it has currently made.** Battery present → the measured bus. CV bench
  → the setpoint. Without this the ceiling chases its own output; see 030.

### Modes and states are separate axes

**Mode** is what was asked for; **state** is where the channel is.

| Mode | Regulates | Via | Sign |
|---|---|---|---|
| MANUAL | nothing | host sets duty directly | — |
| CV | bus voltage → setpoint | duty | duty ↑ = Vout ↑ |
| MPPT | Vin → setpoint chosen by P&O | duty | duty ↑ = Vin ↓ |

CV is a **bench mode for developing control algorithms on a single channel**
into a resistive or electronic load. Five channels regulating one shared bus
would fight; only one runs in CV at a time.

Per channel:

```
   STOPPED --start--> STARTING --conduction + bus ok + dwell--> ACTIVE
      ^                   |                                      |
      |                   +---- ramp timeout ----+               |
      |                                          |               |
      +--------- clear ------- FAULTED <---------+------ OCP/OVP -+
```

`STARTING` is open-loop only — ramp from 0 %, no integrator anywhere. It exists
because while the ideal diode blocks, the channel cannot move the measured bus
at all, so a closed loop there winds up against a constant error and slams on
conduction. Conduction is detected on the channel's own **output current**,
never on a voltage. `ACTIVE` is where the mode's regulator runs.

### System state — arming, and nothing more

"Normal operation vs a serial dev mode" collapses to one bit: **is the
autonomous sequencer allowed to run?** Everything else that distinguishes them
is already the per-channel mode axis. That single bit is what justifies a
system-level FSM at all, and is all it decides.

```
  INIT --> CHECK --> STANDBY --armed--> RUN
                        ^                |
                        +---- hold ------+

              FAULT <-- OVP latched -- (any state)
                +-- clear --> STANDBY
```

- **INIT** — peripherals up, nothing switching, nothing measured.
- **CHECK** — bounded window. Exits on a **checklist, not a clock**: one good
  INA228 sweep, a settled bus reading, fault lines sampled at rest, and the
  `iind` zero calibration, which [decisions.md](decisions.md) 028 says is only
  valid while every channel is stopped. A timeout is the backstop, not the
  condition.
- **STANDBY** — everything known, nothing switching. Safe to sit in
  indefinitely. **This is the dev mode.**
- **RUN** — the sequencer brings channels up and hands them to MPPT. Its one
  non-obvious job is **staggering** starts: five simultaneous ramps into one
  shared bus is five inrush events against a global OVP.
- **FAULT** — OVP latched. System-level because it is global and makes every
  start refuse. OCP stays per-channel; one channel tripping must not take the
  other four down.

**CHECK auto-advances to RUN unless a `hold` lands during the window.** No
command, no host, no connection — it arms. See 031 for why the default points
this way and why serial presence must not be the trigger.

**There is no auto-rearm.** A board in `hold` with a probe on the power stage
must never decide on its own to start switching. The mitigation for "left in
hold accidentally" is to make hold loud, not to make it expire.

### Roadmap

Rough order, not a commitment. Steps 1–3 give a working, tunable converter with
no MPPT in it; step 4 is then small.

1. **The duty gate.** Slew limit (50 %/s) and dynamic ceiling, in `control.c`.
   Verifiable on a scope with no panel. Closes 009 and 012.
2. **`STARTING` ramp.** 0 % to ideal-diode conduction, reliably. The hardest
   bench milestone, and it needs no regulator at all.
3. **CV loop.** Single channel into an electronic load. This is where control
   algorithms actually get written and tuned.
4. **MPPT.** P&O on the Vin setpoint, on top of the same slot. `Src/app/mppt.c`
   stays a pure function of measurements → setpoint, so it is swappable and
   testable on the host.
5. **`INIT`/`CHECK`/`STANDBY`**, then the `RUN` sequencer once there is a
   working converter to drive with it.
6. **Analog build-out.** NTC and fast current sensing need an ADC plan
   ([hardware.md](hardware.md) open question 4).
7. **FDCAN.** Telemetry broadcast to the car computer on FDCAN1.
8. **EL injection mode.** `INJECT_EN` bypasses the ideal diode to push current
   back into the panels. Held low until then.

### Open questions

1. **Is `hold` accepted at any time, or only during CHECK and RUN?** Always is
   simpler and safer at the bench; against it, a mid-drive CAN glitch could stop
   power generation.
2. **When a channel faults in RUN, does the sequencer retry it?** Never, a
   bounded number of times, or after a cooldown? Isc is below the OCP threshold,
   so per 012 every trip is a transient — which argues a retry is usually right,
   and equally that an unbounded retry loop would hide a real fault.
3. **Numbers still TBD**, all to be parameterised in `config.h`: ceiling margin,
   conduction-detect threshold, bus floor for handover, startup dwell, ramp
   timeout, CV setpoint cap and gains, MPPT step size and settle time.
