# Plan

Intent, working rules, and where the firmware is going. Board facts live in
[hardware.md](hardware.md); build and style in [workflow.md](workflow.md). The
code is the authority on what exists today; this file is the shape it is
growing into, with everything unbuilt marked **TODO**.

## Introduction (DO NOT EDIT)
This is the firmware for a GaN MPPT device with 5 seperate channels. I am writing this document because I feel like I do not understand the way the firmware is going, nor do you understand what I want this to be.

## Agent Instructions (DO NOT EDIT)
This is an eductional experience, and in fact my first introduction to designing firware from the ground up. DO NOT EDIT CODE unless explicitly told you are able to. If you are unsure, ask me.

Simplicity and visibility is the goal here, continue on with the current folder structure. Lean towards simple and verifiable solutions, rather then complex optimisations. Saftey is critical, firmware functions that cause the PWM duty cycle to go high have the potential to damage the board itself, although this is unlikely with the current interrupts.

I would prefer to use cube mx to configure pin settings, do not configure these yourself unless necessary, and instead tell me what to change in cube mx. Additionally, DO NOT edit the cube mx generated code in any way. You are only allowed to write inside the cube functions.

Append additional context to this file, do not edit sections with "DO NOT EDIT" unless we discuss and deem it to be useful.

This file alongisde the codebase is the main source of truth, and anything else in .agents. Consider the fact that I will be using both GPT5.6 SOL and CLAUDE OPUS 5 in writing this, so avoid model specific memory locations, and instead make a new file inside .agents to store information.

## Working Agreement

Agreed 2026-08-15. The sections above are unchanged.

- **State the interface before writing the implementation.** Name the
  functions, the types, and the invariant each one protects, and get agreement
  before producing code. Without a hard gate, agents drift into writing the
  whole module instead of the bare interface asked for above.
- **Say whether a change was verified or only compiled.** There is no automated
  test suite; verification is manual, on hardware. An agent can only get code
  to build. Never report a hardware behaviour as confirmed.
- **Numbers carry their arithmetic.** Any limit, threshold or cadence is
  written down with the calculation that produced it — in the comment next to
  it, and in [hardware.md](hardware.md) if it comes from the board.
- **CubeMX changes are instructions, not actions.** Say what to change in the
  `.ioc` and stop.
- **Where new context goes:** board facts → [hardware.md](hardware.md); build
  and style → [workflow.md](workflow.md); intent and architecture → this file.
  Nothing project-specific goes into a model's private memory.

## Firmware Architecture

Simplicity and visibility over clever optimisation, with room to experiment
with control algorithms. Tunable numbers live in `Inc/config.h`; anything that
follows from the hardware and can never change is hardcoded next to the code
that uses it.

```
Inc/config.h                  every tunable number, no includes/types/logic
Inc/drivers/  Src/drivers/    hardware-facing modules
Inc/app/      Src/app/        control and high-level logic
```

### The loop

`main()` calls `app_setup()` once, then `app_loop()` forever. `app_loop()` runs
the system state machine, then the services that run in every state:

```
app_loop()
  switch (sys.state)     INIT -> CHECK -> STANDBY, plus ACTIVE and FAULTED
  analog_service()       bus voltage off ADC1, every ANALOG_PERIOD_MS
  status_service()       LEDs from state; pure reads, no I/O
  telem_service()        advances one INA228 I2C transfer per pass
```

**Nothing in the loop blocks.** Every service is written to be called often and
return immediately — I2C transfers are sequenced across passes, ADC conversions
are sub-microsecond one-shots. Anything new follows the same rule.

### Where state lives

Two places, and they are where to look for anything:

- **`sys`** (`Inc/app/system.h`) — anything true of the whole board: `vbus_mv`,
  the OVP latch, the system state.
- **`channel_a` .. `channel_e`** (`Inc/app/channel.h`) — everything about one
  channel: its telemetry sample, its PWM configuration and operating state.

Drivers publish into these structs and keep no second copy. `channel_x.pwm`
already carries `duty_commanded` alongside `duty_applied` — asked-for and
actual, side by side, which is the pattern the rest of the control path
follows.

### The system state machine

The `switch` in `app_loop()` is the machine; `system_state_t` is the list of
states. `INIT → CHECK → STANDBY` runs today, with the bring-up checklist in
`check.c`.

`FAULTED` is reachable from any state, stops all five channels on entry, and is
left **only by rebooting** — no clear-in-place, no auto-recovery. OVP latches
it. Per-channel OCP does **not**: one channel tripping must not take the other
four down.

**TODO — STANDBY has no exit and ACTIVE is empty.** Nothing arms itself: the
board leaves STANDBY only on a command from the car computer over FDCAN, or the
same command over serial. That is deliberate. The car computer is the thing
that knows the battery's state, whether the car is running, and whether
charging is wanted at all, so the firmware does not guess at a decision another
node already owns.

**There is one run state, `ACTIVE`, and a system-level mode inside it.**
Decided 2026-08-23, replacing an earlier design that made each mode its own
system state (`RUN_MPPT`, `RUN_CV`, …). Many more modes are expected, and five
near-identical states differing only in which regulator runs is the worse
diagram.

The objection to a mode field was that modes differ in what they *permit*, not
just in which regulator runs — so a field inside one state silently changes
what that state means. **`mode.c` is the answer to that**: the permissions
belong to the mode, not to `sys`. See "Modes" below.

**TODO — the CHECK checklist is partly stubbed.** `check.c` reads the five FLT
lines and OVP; the INA228 sweep and the inductor zero calibration return true
without doing anything.

### The channel state machine — TODO, not designed

One per channel, five copies, no shared state, running underneath whichever run
state the system is in. The sketch so far:

```
   STOPPED --start--> STARTING --conduction + bus ok + dwell--> ACTIVE
      ^                   |                                      |
      |                   +---- ramp timeout ----+               |
      +--------- clear ------- FAULTED <---------+------ OCP ----+
```

`STARTING` is **open-loop only** — ramp from 0 %, no integrator anywhere. It
exists because while the ideal diode blocks, the channel cannot move the
measured bus at all, so a closed loop there winds up against a constant error
and slams on conduction. Conduction is detected on the channel's own **output
current**, never on a voltage. `ACTIVE` is where the mode's regulator runs.

### The control stack — TODO, none of it is written

```
  SOURCES        GUI / serial        MPPT algorithm        FDCAN
                      |                    |                  |
                      +----------+---------+------------------+
                                 v
  INTENT         per-channel mode + state machine + setpoint
                 picks a duty; never touches hardware
                                 v
  CONDITIONING   duty gate: slew limit -> dynamic ceiling
                 every duty from every source passes here
                                 v
  APPLY          batches pending changes, one stop/start bracket
                 the only caller of pwm_*()
                                 v
  DRIVER         pwm.c: static clamps, fault latches, output enable
                                 v
  SILICON        HRTIM: OCP forces outputs inactive in hardware
```

Three rules hold it together, and they are the load-bearing part of this file:

- **One module applies duty.** Every command source — serial, FDCAN, MPPT —
  posts a request and returns; a single service applies pending requests once
  per pass and is the only code that calls `pwm_*()`. Several sources can then
  command one channel without racing inside the driver, and a burst of requests
  applies as one batch in one stop/start bracket, so a frequency change followed
  by a duty change glitches a live output once instead of twice. Rejection stays
  **synchronous**, so a transport can answer its host immediately.
- **The gate is unconditional.** Slider, MPPT, CV loop, CAN — every duty is
  slew-limited and ceiling-clamped in one place, with no path around it.
- **`Vbus_ref` is the voltage the channel is *trying to reach*,** never the
  voltage it has currently made: the measured bus when a battery is present, the
  setpoint on a CV bench. Otherwise the dynamic ceiling chases its own output.

`pwm.c` is the safety boundary underneath all of it: static clamps, fault
latches, and the critical sections around output enable/disable.

### Commands — TODO

The car computer drives the board over FDCAN, and **serial carries the same
message set**: same commands, same arguments, same replies. Neither transport is
privileged, so the bench exercises the real control path rather than a parallel
debug one. One command type, two codecs — the transports themselves are drivers,
the decode and dispatch is app code.

An earlier serial-only implementation was removed in `efaf6bb`. `git show
efaf6bb^:Inc/app/command.h` has the grammar it spoke, and the host tools in
`tools/` still speak it.

### Modes

**A mode is a property of the whole system, not of a channel.** One runs at a
time, inside `ACTIVE`, and it handles all five channels. Three so far, many
more expected; each lives in `Src/app/modes/`.

| Mode | Regulates | Via | Sign |
|---|---|---|---|
| MANUAL | nothing | host sets duty directly | — |
| CV | bus voltage → setpoint | duty | duty ↑ = Vout ↑ |
| MPPT | Vin → setpoint chosen by P&O | duty | duty ↑ = Vin ↓ |

CV is a **bench mode for developing control algorithms on a single channel**
into a resistive or electronic load. Five channels regulating one shared bus
would fight, so only one channel runs and the mode keeps the other four
stopped. Which channel is a compile-time constant in `config.h` — CV is a bench
mode, so a rebuild to change it is not a cost worth carrying a mutable field
for.

**Mode is still not state.** Mode is what was asked for; state is where a
channel is. A channel sitting stopped while the system is in MPPT mode is a
normal thing to be.

#### `mode.c` dispatches; the mode owns the channels

`app.c` calls `mode_begin()` on entry to `ACTIVE` and `mode_service()` every
pass, and never learns which mode is running. Adding a mode touches
`Src/app/modes/`, not the system state machine.

```c
bool         mode_begin(mode_t mode);   /* false = refused, nothing started */
mode_state_t mode_service(void);        /* INIT / RUNNING / FAULTED / EXIT   */
void         mode_stop(void);           /* the one teardown, every exit path */
```

Three invariants, and they are why this is a module rather than a `switch` in
`app.c`:

- **The running mode is the sole owner of all five channels while `ACTIVE`.**
  Nothing outside it may command a channel — not the command layer, not MPPT
  reaching in sideways. Requests arrive; the mode decides what to do with them.
  To know what any channel is being asked to do, read one mode's code.
- **One teardown.** Finish, fault or stop all end through `mode_stop()`, so the
  path that leaves channels switching cannot be forgotten in a mode added
  later.
- **The mode is latched at `mode_begin()`.** A later write to `sys.mode` cannot
  swap regulators underneath a live channel; it is picked up at the next start.

`mode_begin()` returns `bool` because a run command must be refusable — wrong
channel state, bus absent, a fault latched — and refusal has to be synchronous
so a transport can answer its host immediately.

A `switch` inside `mode.c` is right at three modes. Around six, when the same
switch appears in `begin`, `service` and `stop`, it becomes a `static const`
table of `{begin, service, stop}` and each new mode is one row. Do not start
there.

## Roadmap

Rough order, not a commitment. Steps 2–4 give a working, tunable converter with
no MPPT in it; step 5 is then small.

1. **Commands in, and the mode they select.** Nothing can leave STANDBY until
   this exists, so it gates everything below it.
2. **The duty gate.** Slew limit and dynamic ceiling, in one place. Verifiable
   on a scope with no panel connected.
3. **`STARTING` ramp.** 0 % to ideal-diode conduction, reliably. The hardest
   bench milestone, and it needs no regulator at all.
4. **CV loop.** Single channel into an electronic load. This is where control
   algorithms actually get written and tuned.
5. **MPPT.** P&O on the Vin setpoint, on top of the same slot. Kept a pure
   function of measurements → setpoint, so it is swappable and testable on the
   host.
6. **Inductor current sensing.** Removed along with `iind.c`; needs the ADC2 /
   ADC3 CubeMX work listed in [hardware.md](hardware.md).
7. **Temperature.** Nothing consumes it until the divider is understood — see
   [hardware.md](hardware.md).
8. **FDCAN.** Telemetry broadcast to the car computer on FDCAN1.
9. **EL injection mode.** `INJECT_EN` bypasses the ideal diode to push current
   back into the panels. Held low until then.

## Open questions

Board questions live in [hardware.md](hardware.md). These are the control ones.

1. **Numbers still TBD**, all destined for `config.h`: ceiling margin, bus floor
   for handover, CV setpoint cap and gains, MPPT step size and settle time,
   start stagger interval, conduction-detect threshold, startup dwell, ramp
   timeout, bus settle window and count.
2. **Is a failed check really fatal?** One dead INA228 would send the board to
   FAULTED, and a reboot will not fix a dead sensor. Running four channels may
   well beat running none — but that needs a way to declare a channel absent,
   which is not designed.
3. **What happens in a run state when the bus disappears?** Back to STANDBY, or
   a fault?
4. **Does losing the CAN link in a run state mean anything?** Keep generating,
   or fall back to STANDBY.
5. **When a channel faults in a run state, does it get retried?** Never, a
   bounded number of times, or after a cooldown? Isc is below the OCP threshold,
   so every trip is a transient — which argues a retry is usually right, and
   equally that an unbounded retry loop would hide a real fault.
