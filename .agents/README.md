# .agents — orientation and architecture

**The code is the authority on what the firmware does, and
[todo.md](todo.md) is the authority on what is outstanding.** These files carry
what those two cannot: board facts, the working rules, the build loop, and the
shape the firmware is growing into. They are written to be read by more than one
model (Claude Opus and GPT are both used here), so nothing lives in a model's
private memory.

| File | What it is | Read it when |
|---|---|---|
| [project_agent_instructions.md](project_agent_instructions.md) | The author's instructions and the working agreement | **Always, first.** |
| This file | File index, firmware architecture, open questions | Before changing how anything fits together |
| [hardware.md](hardware.md) | Board facts: converters, sensors, limits, pin/fault mapping, derived safety numbers | Before touching anything that drives or measures hardware |
| [workflow.md](workflow.md) | Build, flash, bench test loop, code layout, coding conventions | Before building, flashing or writing code |
| [todo.md](todo.md) | The project todo list, in the author's words | For what is outstanding. **Never implement from it unprompted.** |
| [thesis_discussion_points.md](thesis_discussion_points.md) | Critical revelations: decisions that changed, assumptions that broke | When something non-obvious turns up, or when writing about the project |

## Rules for maintaining these files

- **Facts go in [hardware.md](hardware.md).** Anything measurable about the
  board. If a number here disagrees with the code, one of them is a bug — say
  so rather than silently trusting either.
- **Do not restate the code.** These files say what the code cannot say about
  itself. Where a doc and the code disagree about what exists, the code wins
  and the doc gets fixed.
- **Unfinished work goes in [todo.md](todo.md)**, never here. Task ids are
  fixed; new tasks take the next free number and gaps are fine. `TODO` comments
  still belong **in the code**, next to the line they concern — that is where
  they keep their context. Never work from todo.md unprompted.
- **[thesis_discussion_points.md](thesis_discussion_points.md) is for critical
  revelations, not progress.** Add an entry when something changed a design
  decision, contradicted an assumption, or is a general lesson rather than a
  project detail — and record the evidence, not just the conclusion.
- **Sections marked `DO NOT EDIT` are the author's words.** Do not reword,
  reformat or "improve" them. Propose changes in conversation instead.
- **Open questions stay visible.** Unanswered items live in the `Open questions`
  section below rather than being guessed at and buried in prose.
- **If you assume something, write the assumption down** in the same place the
  code that depends on it lives.

---

# Firmware architecture

Simplicity and visibility over clever optimisation, with room to experiment
with control algorithms. Folder layout and the `config.h` rule are in
[workflow.md](workflow.md#code-layout).

## The loop

`main()` calls `app_setup()` once, then `app_loop()` forever. `app_loop()` runs
the system state machine, then the services that run in every state:

```
app_loop()
  command_service()    drain transports -> command state
  switch (sys.state)          INIT -> CHECK -> STANDBY -> ACTIVE, plus FAULTED, RESET
  analog_service()            bus voltage off ADC1, every ANALOG_PERIOD_MS
  status_service()            LEDs from state; pure reads, no I/O
  telem_service()             advances one INA228 I2C transfer per pass
  command_flush_all()  clear commands; anything unacted on is discarded
```

The command service and flush **straddle** the state machine deliberately, and
merging or reordering them breaks it — see [Commands](#commands).

**Nothing in the loop blocks.** Every service is written to be called often and
return immediately — I2C transfers are sequenced across passes, ADC conversions
are sub-microsecond one-shots. Anything new follows the same rule. Nothing
bounds the loop period, so no design should depend on a maximum pass duration.

## Where state lives

Two places, and they are where to look for anything:

- **`sys`** (`Inc/app/system.h`) — anything true of the whole board: `vbus_mv`,
  the OVP latch, the system state.
- **`channel_a` .. `channel_e`** (`Inc/app/channel.h`) — everything about one
  channel: its telemetry sample, its PWM configuration and operating state.

Drivers publish into these structs and keep no second copy: `channel_x.pwm`
carries `duty_applied`, written by `pwm.c` after it programs the compare
register, so a command the driver rejected shows up as the duty not moving. A
`duty_commanded` field existed and was removed — nothing wrote it. The
asked-for/actual pair arrives with the duty gate, which is what will have
something to put in it.

## The system state machine

The `switch` in `app_loop()` is the machine; `system_state_t` is the list of
states. `INIT → CHECK → STANDBY` runs today, with the bring-up checklist in
`check.c`.

Entry behaviour is driven by an `entered` flag derived from the transition
itself, not by separate entry states. That makes it structurally impossible to
reach a state without its entry behaviour running — a guarantee that explicit
`ENTER_*` states would downgrade to a convention.

`FAULTED` stops all five channels on entry and is left only on an explicit
`CLEAR_FAULT`, which routes back through `CHECK` so the board re-runs its
bring-up before it can start again. It does not self-clear. Per-channel OCP
does **not** reach it: one channel tripping must not take the other four down.

`RESET` is a state rather than an inline call, so the reset is visible in the
machine: its entry stops the outputs, then resets the MCU. A `RESET` command is
honoured from every state, checked once before the switch.

**The board never arms itself.** It leaves STANDBY only on a command from the
car computer over FDCAN, or the same command over serial. That is deliberate —
the car computer knows the battery's state, whether the car is running, and
whether charging is wanted at all, so the firmware does not guess at a decision
another node already owns.

**There is one run state, `ACTIVE`, and a system-level mode inside it.**
Decided 2026-08-23, replacing an earlier design that made each mode its own
system state (`RUN_MPPT`, `RUN_CV`, …). Many more modes are expected, and five
near-identical states differing only in which regulator runs is the worse
diagram. The objection to a mode field was that modes differ in what they
*permit*, not just in which regulator runs — **`mode.c` is the answer to that**:
the permissions belong to the mode, not to `sys`.

## The channel state machine — not designed

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

## The control stack — mostly unwritten

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

`Src/app/control/` holds the pieces: a generalised PI controller (`pi.c`), the
P&O algorithm (`perturb_observe.c`), and the gate (`control.c`). Only `pi.c` is
written.

## Commands

The car computer drives the board over FDCAN, and **serial carries the same
message set**. Neither transport is privileged, so the bench exercises the real
control path rather than a parallel debug one.

The consumer side is built (`Inc/app/command.h`, `Src/app/command.c`); **no
transport exists yet**, so nothing feeds it.

```c
void   command_init(void);                           /* clean slate at startup    */
mode_t system_command_requested_mode(void);          /* MODE_NONE if none pending */
bool   system_command_received(system_commands_t c); /* pure read, no side effect */
void   command_service(void);                 /* drain transports; only setter */
void   command_flush_all(void);               /* the only clearer          */
```

**Commands are split by consumer, and that split is what makes the design
work.** System commands (`system_commands_t`) are consumed only by `app.c`;
mode commands will be consumed only by the running mode.

Five invariants hold it together:

- **Within one pass the command state is immutable.** `service()` sets it at
  the top, the state machine reads it as many times as it likes, `flush_all()`
  clears it at the bottom. Reads are pure, so two consumers polling the same
  command get the same answer in either order, and adding a consumer needs no
  arbitration.
- **The two calls must straddle the state machine.** Both at the bottom and
  every command is a pass stale; `flush_all()` anywhere before the switch and
  commands are wiped before any state sees them. Both failures are silent.
- **`flush_all()` is the only thing that clears.** `service()` only ever sets.
  That stays true no matter who ends up setting flags later.
- **Interrupts deliver bytes, never commands.** A flag set from an ISR could
  land after the state machine has run and be cleared before anything saw it.
  Keeping the flags single-writer — main loop only — also removes every
  atomicity question; the transport ring is the only concurrent structure.
- **Transports are drained, not polled per command.** Each transport hands
  commands over one at a time in arrival order (`*_take_next_system_command()`),
  which is what makes `pending_mode` last-write-wins mean *most recent* rather
  than *highest enum value*. The take also clears the driver's copy, so a
  command arriving mid-pass survives to the next one.

**Serial outranks CAN for mode selection**, because serial is the manual
interface and a human should beat an automated bus master. It is enforced by
draining CAN first and serial second, so the drain order *is* the rule — which
reads backwards and must not be reordered. Note this is a tie-break within one
pass, not a persistent manual lockout: a chatty CAN master takes the mode back
on the next pass.

Only `pending_mode` needs arbitrating. Everything else is a flag, and flags OR
together — `STOP` from either bus is `STOP`.

An earlier serial-only implementation was removed in `efaf6bb`. `git show
efaf6bb^:Inc/app/command.h` has the grammar it spoke.

**Mode commands are a different shape and do not belong in this register.**
`set vout 25000` is a value that persists until changed, and the
flush-every-pass lifetime here would delete it a millisecond later. Values want
their own store; events belong here.

## Modes

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

### `mode.c` dispatches; the mode owns the channels

`app.c` calls `mode_begin()` on entry to `ACTIVE` and `mode_service()` every
pass, and never learns which mode is running. Adding a mode touches
`Src/app/modes/`, not the system state machine.

```c
mode_init_result_t mode_begin(mode_t mode);         /* REFUSED / FAULT / OK */
mode_state_t       mode_service(bool stop_request); /* INIT / RUNNING / FAULTED / EXIT */
```

There is no public teardown. A thing nobody outside can call is harder to
forget than a thing everybody must remember, so `mode.c` tears down internally
on every exit path and `app.c` never does it.

`stop_request` is an **edge** — `flush_all()` clears the STOP command at the
bottom of the pass it arrived in — so `mode.c` latches it and hands the mode
below a **level** that stays true until it exits. An individual mode therefore
has nothing to latch and cannot drop a stop by missing a pass. Stopping is
allowed to take several passes; a ramp down to zero duty is the reason for
asking rather than telling. A mode never reads the command layer itself; it is
handed a request as a parameter.

Four invariants, and they are why this is a module rather than a `switch` in
`app.c`:

- **The running mode is the sole owner of all five channels while `ACTIVE`.**
  Nothing outside it may command a channel — not the command layer, not MPPT
  reaching in sideways. Requests arrive; the mode decides what to do with them.
  To know what any channel is being asked to do, read one mode's code.
- **One teardown, and it is internal.** `MODE_STATE_EXIT` and
  `MODE_STATE_FAULTED` both mean the channels are already stopped and the
  internal state already reset. `app.c` tears nothing down; the `pwm_stop_all()`
  on entry to STANDBY and FAULTED is a backstop against a buggy mode, not the
  mechanism.
- **`mode_begin()` initialises fully and cleans up after itself.** It never
  assumes a clean slate — a previous run may have left an integrator wound up —
  and never returns FAULT or REFUSED with a channel still switching.
- **The mode is latched at `mode_begin()`.** `mode.c` holds it rather than
  re-reading `sys.mode` each pass, so a later write cannot swap regulators
  underneath a live channel; it is picked up at the next start.

`mode_begin()` is refusable — wrong channel state, bus absent, a fault latched —
and refusal is synchronous so a transport can answer its host immediately. It is
also one-shot, which means a *transient* refusal currently loses the command;
splitting the checks into their own state is task 013.

A `switch` inside `mode.c` is right at three modes. Around six it becomes a
`static const` table of `{begin, service}` and each new mode is one row. Do not
start there. Neither switch carries a `default:`, so adding a `mode_t` fails to
build until it is wired into both.

## Direction

[todo.md](todo.md) is the work queue. The shape of the next stretch: inductor
current sensing sits **ahead of MPPT**, because it is the foundation for a
current limit that transfers from bench to array, and it makes both the CV and
MPPT inner loops easier rather than harder. MPPT itself stays a pure function of
measurements → setpoint, so it is swappable and testable on the host;
regulating Vin is the *easier* of the two loops, since the right-half-plane zero
lives in the duty-to-output path, not duty-to-input.

Further out and not queued: temperature (nothing consumes it until the divider
is understood — see [hardware.md](hardware.md)), FDCAN telemetry broadcast to
the car computer, and EL injection mode, where `INJECT_EN` bypasses the ideal
diode to push current back into the panels. `INJECT_EN` is held low until then.

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
   bounded number of times, or after a cooldown? On the array Isc is below the
   OCP threshold, so every trip is a transient — which argues a retry is usually
   right, and equally that an unbounded retry loop would hide a real fault. That
   reasoning is array-specific and does not hold on a stiff bench supply.
6. **Should the control step be timer-driven or event-driven?** Running it when
   a fresh measurement lands, rather than on a fixed period, removes the beat
   between the ADC and control timers and makes `dt` exactly the sampling
   interval. Matters more for MPPT, where the measurement is the slower INA228.
7. **Does a command nobody acted on need an answer?** `flush_all()` is where a
   rejection would be issued. Judged not critical while serial is the only
   interface and the board's behaviour is visible on the bench; it becomes real
   when a CAN master needs to know its command was dropped.
