# User todo list
Agent, do not implement these, it is for myself to keep track. If I ask you to add a task, keep it very clear and simple. Use simple language and do not make it overly verbose.

This file is the project todo list. Unfinished work goes here, not in
project_plan.md. `TODO` comments still belong in the code, next to the line
they concern.

# Tasks
## Managing fault clearing and recovery states.
Clearing fault states, when do we want to do this, etc. Auto recovery. This also includes a path for interrupts to push the system into faulted.

## Check state needs to check for active system OCP and OVP faults.
Title explains it all.

## Timeouts, put them everywhere
type shit

## Command layer: nothing decodes yet
`command.c` is a temporary stub that asks for CV five seconds after boot.
No transport exists - `serial.c` was removed and `fdcan.c` has no RX.
Needs: a UART ring, a text decoder, and the single-slot decision written down.

## Command replies
Nothing answers the host. `system_command_flush_all()` is where a command
nobody acted on should be rejected. Needs the ack/reject path.

## Duty gate
Slew limit currently lives inside `mode_single_ch_cv.c`. It belongs in
`app/control` once a second mode needs it, together with the dynamic ceiling.

## Fill in app/control
`pi.c` and `perturb_observe.c` are stubs. `control.c` is empty.

## OVP does not reach the state machine
`sys.ovp_latched` is only read inside the CV mode. Nothing checks it in CHECK
or STANDBY.

## CLEAR_FAULT does not clear the latches
It moves the state machine to CHECK but never calls `pwm_clear_OVP_fault()` or
`pwm_clear_OCP_fault()`, so the board reaches STANDBY looking healthy while
every start is still refused.

## Channel state machine
Not designed. Sketch is in project_plan.md.

## Inductor current sensing
CubeMX work is fully specified in hardware.md. Worth moving ahead of MPPT -
it is the foundation for a current limit that transfers from bench to array.

## Slew limit needs scaling by operating point
Tuned at 12 V in / 25 V out with ~10 A of OCP headroom. The array leaves 3.4 A
and a higher bus. Roughly 6x too permissive as written.

## Tidy
- `serial_flush.svg` lands in the repo root each run; wants a .gitignore line.
- `command.c`, `command.h`, `channel.c` are still 4-space indented.
- Stale comments: `RUN_TIME_MS // 6 seconds`, `START_HOLD_TIME_MS // 5 seconds`,
  and the gain-derivation block in `mode_single_ch_cv.c` still describes the
  old numbers.
