# State machines

Two machines. A **system FSM** for the whole board, and a **channel FSM** per
channel. Neither touches hardware — they decide, `control.c` applies, `pwm.c`
drives the HRTIM.

Why the shape is what it is goes in [decisions.md](decisions.md). This file is
just the map.

**Status: design only. Nothing here is written yet.**

## Commands

The car computer drives the system FSM over FDCAN. **Serial carries the same
message set** — same commands, same arguments, same replies — so anything the
car can ask for can be typed at the bench, and the bench is testing the real
path. Neither transport is privileged.

---

# System FSM

```
  INIT --> CHECK --> STANDBY  -- run mppt -->  RUN_MPPT
                        ^      -- run cv   -->  RUN_CV
                        |      --   ...    -->  (more to come)
                        |                           |
                        +----------- stop ----------+

  (any state) -- fault --> FAULT -- reboot --> INIT
```

Run states will grow. `RUN_MPPT` and `RUN_CV` are the two known today.

## INIT

Peripherals come up: clocks, HRTIM, I2C, ADC, UART, FDCAN. Channel structs are
zeroed. All five PWM timers are configured and left stopped.

Nothing is switching. Nothing has been measured yet.

### Transitions

| To | On |
|---|---|
| CHECK | bring-up finished — unconditional, first pass of the main loop |
| FAULT | a peripheral failed to initialise |

## CHECK

A bounded window used to prove the board is sane. Nothing is switching, which is
what makes this the only place the `iind` zero calibration is valid.

The checklist:

| # | Check |
|---|---|
| 1 | one good INA228 sweep — all ten devices answer |
| 2 | bus voltage settled — N readings inside a window |
| 3 | all five FLT lines and the OVP line read de-asserted |
| 4 | `iind` zero calibration done |

### Transitions

| To | On |
|---|---|
| STANDBY | every check passed |
| FAULT | any check failed, or the window timed out |

## STANDBY

Everything is measured, nothing is switching. Safe to sit in indefinitely.

**The board does not leave this state on its own.** It waits for a command from
the car computer, or the same command over serial.

### Transitions

| To | On |
|---|---|
| RUN_MPPT | `run mppt` command |
| RUN_CV | `run cv <ch>` command |
| FAULT | a system fault latches |

A run command is refused, not queued, if the conditions for that mode are not
met — bus absent, a fault latched. Rejection is immediate so the sender gets an
answer.

## RUN_MPPT

Normal operation. All five channels run their own channel FSM in MPPT mode.
Starts are staggered so five ramps do not hit the shared bus at once.

A channel faulting on OCP stays a channel-level event. It does not leave this
state.

### Transitions

| To | On |
|---|---|
| STANDBY | `stop` command — every channel stopped first |
| FAULT | a system fault latches |

## RUN_CV

Bench mode. **One channel only**, regulating the bus to a voltage setpoint. The
other four stay stopped. Five channels regulating one shared bus would fight
each other, so the state carries which channel is active.

### Transitions

| To | On |
|---|---|
| STANDBY | `stop` command — the channel stopped first |
| FAULT | a system fault latches |

## FAULT

Everything is stopped, every output is off, and any command that would start
switching is refused. Latched.

Entered from any state. What latches it:

- OVP — global, and the only hardware-detected one
- a failed self-check in CHECK
- a peripheral init failure in INIT

Per-channel OCP is **not** a system fault. One channel tripping must not take
the other four down.

### Transitions

| To | On |
|---|---|
| INIT | power cycle, or a `reboot` command over CAN or serial |

There is no automatic recovery and no clear-in-place. Leaving FAULT means
restarting the firmware. Automatic recovery may come later; it is not designed.

---

# Channel FSM

One per channel, five copies, no shared state. Runs underneath whichever run
state the system is in.

**Not worked out yet.** The sketch so far, moved here from
[project_plan.md](project_plan.md):

```
   STOPPED --start--> STARTING --conduction + bus ok + dwell--> ACTIVE
      ^                   |                                      |
      |                   +---- ramp timeout ----+               |
      |                                          |               |
      +--------- clear ------- FAULTED <---------+------ OCP -----+
```

`STARTING` is open-loop only — ramp from 0 %, no integrator anywhere. It exists
because while the ideal diode blocks, the channel cannot move the measured bus
at all, so a closed loop there winds up against a constant error and slams on
conduction. Conduction is detected on the channel's own **output current**,
never on a voltage. `ACTIVE` is where the mode's regulator runs.

To be rewritten in the same format as the system FSM above.

Active run modes may each carry their own sub-FSM. Also not designed.

---

# Open questions

1. **Is a failed check really fatal?** A single dead INA228 currently sends the
   board to FAULT, and a reboot will not fix a dead sensor. Running four
   channels may beat running none.
2. **What are the remaining run states?** Only `RUN_MPPT` and `RUN_CV` are
   named so far.
3. **Does `RUN_CV` need the car computer's permission,** or is it bench-only and
   refused when the CAN link is live?
4. **What happens in a run state when the bus disappears?** Back to STANDBY, or
   a fault?
5. **Does losing the CAN link in RUN_MPPT mean anything?** Keep generating, or
   fall back to STANDBY.
6. **When a channel faults in a run state, does it get retried?** Never, a
   bounded number of times, or after a cooldown? Isc is below the OCP
   threshold, so per [decisions.md](decisions.md) 012 every trip is a
   transient — which argues a retry is usually right, and equally that an
   unbounded retry loop would hide a real fault.
7. **Numbers, all TBD, all destined for `config.h`:** CHECK timeout, bus settle
   window and count, bus-present threshold, start stagger interval,
   conduction-detect threshold, startup dwell, ramp timeout.
