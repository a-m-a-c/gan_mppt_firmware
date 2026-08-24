# User todo list
Agent, do not implement these, it is for myself to keep track. If I ask you to add a task, keep it very clear and simple. Use simple language and do not make it overly verbose.

This file is the project todo list. Unfinished work goes here, not in the
other .agents files. `TODO` comments still belong in the code, next to the line
they concern.

Task ids are fixed. New tasks take the next free number, gaps are fine.

# Tasks
## 001 - Fault paths through the central FSM
General task. How a fault gets into the state machine and how it gets out.
Clearing fault states, when do we want to do this, etc. Auto recovery. This also includes a path for interrupts to push the system into faulted.
Two known gaps to fold in:
- `sys.ovp_latched` is only read inside the CV mode. Nothing checks it in CHECK
  or STANDBY.
- CLEAR_FAULT moves the state machine to CHECK but never calls
  `pwm_clear_OVP_fault()` or `pwm_clear_OCP_fault()`, so the board reaches
  STANDBY looking healthy while every start is still refused.

## 002 - Check state needs to check for active system OCP and OVP faults.
Title explains it all.

## 003 - Timeouts, put them everywhere
type shit

## 006 - Implement the control gate in control.c
Every duty from every source goes through one gate: slew limit, then dynamic
ceiling. No path around it.
The slew limit lives inside `mode_single_ch_cv.c` right now.
It also needs scaling by operating point. Tuned at 12 V in / 25 V out with
~10 A of OCP headroom. The array leaves 3.4 A and a higher bus, so it is
roughly 6x too permissive as written.

## 011 - Inductor current sensing
CubeMX work is fully specified in hardware.md. Worth moving ahead of MPPT -
it is the foundation for a current limit that transfers from bench to array.

## 013 - Split the mode start checks into their own state
Add a state between STANDBY and ACTIVE, something like SYSTEM_STATE_ACTIVE_CHECK.
It runs the refusal checks that currently sit inside mode_begin, so mode_begin
is left doing only setup. Same shape as CHECK - it can take several passes
instead of having to pass or fail in one.
Right now a refusal loses the command. pending_mode is flushed by then, so
nothing retries and the command has to be sent again.
The check state needs to know what each mode requires, so mode.h probably
grows a mode_check(mode) next to mode_begin(mode). Decide that when building
it, not now.

## 014 - Tidy
- `command.c`, `command.h`, `channel.c` are still 4-space indented.
- Stale comments: `RUN_TIME_MS // 6 seconds`, `START_HOLD_TIME_MS // 5 seconds`,
  and the gain-derivation block in `mode_single_ch_cv.c` still describes the
  old numbers.

## 015 - Serial layer implementation
The command consumer side is done, nothing feeds it. Needs a UART5 RX ring
filled from `HAL_UART_RxCpltCallback` in `interrupts.c`, a line decoder, and
`serial_take_next_system_command()`.
Serial outranks CAN for mode selection. That is enforced by draining CAN first
and serial second, so the drain order is the rule - do not reorder it.

## 016 - CAN layer implementation
Same command path as serial, via `can_take_next_system_command()`.
FDCAN bit timing in `fdcan.c` is still at CubeMX defaults (TimeSeg1 and
TimeSeg2 both 1), so the bitrate is not a real number yet.

## 017 - Complete perturb_observe.c
Same format as pi.c. Settle the header interface first, then fill the
functions.

## 018 - Zero the duty cycle in pwm_stop
`pwm_start()` now sets duty to zero before enabling the outputs, which fixed
restarting a channel at the last running duty. Doing the same in `pwm_stop()`
would leave a stopped channel safe rather than relying on the next start to
fix it.
