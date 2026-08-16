# .agents — source of truth

This directory and the code are the only authoritative context for this
project. It is written to be read by more than one model (Claude Opus and GPT
are both used here), so nothing lives in a model's private memory.

## Read order

| File | What it is | Read it when |
|---|---|---|
| [project_plan.md](project_plan.md) | Intent, agent working rules, firmware architecture | **Always, first.** |
| [hardware.md](hardware.md) | Board facts: converters, sensors, limits, pin/fault mapping, derived safety numbers | Before touching anything that drives or measures hardware |
| [workflow.md](workflow.md) | Build, flash, serial, bench test loop, coding conventions | Before building, flashing or writing code |
| [decisions.md](decisions.md) | Append-only log of what was decided and why | Before proposing a change to existing structure |

`image.png` is the original photo of the INA228 address list. The addresses are
transcribed as text in [hardware.md](hardware.md) — prefer the text.

## Rules for maintaining these files

- **Facts go in `hardware.md`.** Anything measurable about the board. If a
  number here disagrees with the code, one of them is a bug — say so.
- **Reasoning goes in `decisions.md`.** Append only, newest at the bottom, with
  a date. Never rewrite an old entry; add a new one that supersedes it.
- **Sections marked `DO NOT EDIT` are the author's words.** Do not reword,
  reformat or "improve" them. Propose changes in conversation instead.
- **Open questions stay visible.** Unanswered items live in a `Open questions`
  section rather than being guessed at and buried in prose.
- **If you assume something, write the assumption down** in the same place the
  code that depends on it lives.
