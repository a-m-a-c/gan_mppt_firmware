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

### The state machine — to be designed

The goal is a system of clear, distinct states. This is **not settled** and is
the next thing to work through.

What exists is a *per-channel* state in `pwm.h` — `UNINITIALIZED`, `STOPPED`,
`RUNNING`, `FAULTED`, where `FAULTED` is derived from the latched fault flags
rather than stored. There is no system-level state anywhere.

Open, for discussion:

- Does a channel need a `STARTING` state for the soft-start ramp, and a
  `TRACKING` state for MPPT? [decisions.md](decisions.md) 010 says a channel
  must start at 0 % and ramp until the ideal diode conducts — that ramp is a
  state, and nothing currently owns it.
- Is there a system-level FSM above the channels (`INIT` / `IDLE` / `ACTIVE` /
  `FAULT`), and if so what does it decide that the per-channel states do not?
- Who owns the transitions — `control.c`, or a new module?
- Where does the ramp rate limiter live ([decisions.md](decisions.md) 012)?
- Where does the dynamic duty ceiling live ([decisions.md](decisions.md) 009)?

### Roadmap

Rough order, not a commitment.

1. **Safety before power.** Bench-verify output polarity, then the soft-start
   ramp and the ramp rate limiter.
2. **The state machine.** Settle the states and transitions above.
3. **MPPT.** `Src/app/mppt.c` is an empty placeholder. Algorithm choice is
   open — the INA228s, not the fast analog sensors, are the current source.
4. **Analog build-out.** NTC and fast current sensing need an ADC plan
   ([hardware.md](hardware.md) open question 4).
5. **FDCAN.** Telemetry broadcast to the car computer on FDCAN1.
6. **EL injection mode.** `INJECT_EN` bypasses the ideal diode to push current
   back into the panels. Held low until then.
