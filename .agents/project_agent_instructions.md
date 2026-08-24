# Agent instructions

How to work in this repo.

**The code and [todo.md](todo.md) are the source of truth.** The code is the
authority on what the firmware does; todo.md is the authority on what is
outstanding. Everything in `.agents/` exists to carry what those two cannot say
about themselves. Where a doc disagrees with the code, the code wins and the
doc gets fixed.

Board facts are in [hardware.md](hardware.md). Architecture and the file index
are in [README.md](README.md). Build, flash, test and style are in
[workflow.md](workflow.md).

## Introduction (DO NOT EDIT)
This is the firmware for a GaN MPPT device with 5 seperate channels. I am writing this document because I feel like I do not understand the way the firmware is going, nor do you understand what I want this to be.

## Agent Instructions (DO NOT EDIT)
This is an eductional experience, and in fact my first introduction to designing firware from the ground up. DO NOT EDIT CODE unless explicitly told you are able to. If you are unsure, ask me.

Simplicity and visibility is the goal here, continue on with the current folder structure. Lean towards simple and verifiable solutions, rather then complex optimisations. Saftey is critical, firmware functions that cause the PWM duty cycle to go high have the potential to damage the board itself, although this is unlikely with the current interrupts.

I would prefer to use cube mx to configure pin settings, do not configure these yourself unless necessary, and instead tell me what to change in cube mx. Additionally, DO NOT edit the cube mx generated code in any way. You are only allowed to write inside the cube functions.

Append additional context to this file, do not edit sections with "DO NOT EDIT" unless we discuss and deem it to be useful.

This file alongisde the codebase is the main source of truth, and anything else in .agents. Consider the fact that I will be using both GPT5.6 SOL and CLAUDE OPUS 5 in writing this, so avoid model specific memory locations, and instead make a new file inside .agents to store information.

 DO NOT use overly verbose comments. code is self documenting and I can read code well. Use // unles /* style is required for longer comments. Often a single line or two is plenty.

## Working Agreement

Agreed 2026-08-15, extended 2026-08-24. The sections above are unchanged.

- **State the interface before writing the implementation.** Name the
  functions, the types, and the invariant each one protects, and get agreement
  before producing code. Without a hard gate, agents drift into writing the
  whole module instead of the bare interface asked for above.
- **Say whether a change was verified or only compiled.** An agent can only get
  code to build; never report a hardware behaviour as confirmed. The bench loop
  is in [workflow.md](workflow.md#testing).
- **Numbers carry their arithmetic.** Any limit, threshold or cadence is
  written down with the calculation that produced it - in the comment next to
  it, and in [hardware.md](hardware.md) if it comes from the board.
- **CubeMX changes are instructions, not actions.** Say what to change in the
  `.ioc` and stop.
- **Read the code before describing it.** These files go stale; the code does
  not. Check the thing you are about to assert still exists.
- **Where new context goes:**
  - board facts -> [hardware.md](hardware.md)
  - outstanding work -> [todo.md](todo.md), never here
  - build, style, conventions -> [workflow.md](workflow.md)
  - architecture and open questions -> [README.md](README.md)
  - a critical revelation -> [thesis_discussion_points.md](thesis_discussion_points.md)

  Nothing project-specific goes into a model's private memory.
- **[thesis_discussion_points.md](thesis_discussion_points.md) is for critical
  revelations only.** An entry belongs there when something changed a design
  decision, contradicted an assumption, or is a general lesson rather than a
  project detail - and it records the evidence, not just the conclusion. Routine
  progress does not go there. Decide whether a finding clears that bar before
  writing it.
