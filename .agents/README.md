# .agents — source of truth

**The code is the authority on what the firmware does.** These files carry what
the code cannot: board facts, the working rules, and the build/test loop. They
are written to be read by more than one model (Claude Opus and GPT are both
used here), so nothing lives in a model's private memory.

| File | What it is | Read it when |
|---|---|---|
| [project_plan.md](project_plan.md) | Intent, agent working rules, architecture, roadmap | **Always, first.** |
| [hardware.md](hardware.md) | Board facts: converters, sensors, limits, pin/fault mapping, derived safety numbers | Before touching anything that drives or measures hardware |
| [workflow.md](workflow.md) | Build, flash, bench test loop, coding conventions | Before building, flashing or writing code |

`NCU18XH103F6SRB.csv` is Murata's characteristic table for the fitted NTC, used
by `tools/gen_ntc_table.py`. `image.png` is the original photo of the INA228
address list — the addresses are transcribed as text in
[hardware.md](hardware.md), prefer the text.

## Rules for maintaining these files

- **Facts go in [hardware.md](hardware.md).** Anything measurable about the
  board. If a number here disagrees with the code, one of them is a bug — say
  so rather than silently trusting either.
- **Do not restate the code.** These files say what the code cannot say about
  itself. Where a doc and the code disagree about what exists, the code wins
  and the doc gets fixed.
- **Unfinished work is marked `TODO` in place,** next to the thing it belongs
  to, not collected into a separate list where it loses its context.
- **Sections marked `DO NOT EDIT` are the author's words.** Do not reword,
  reformat or "improve" them. Propose changes in conversation instead.
- **Open questions stay visible.** Unanswered items live in an `Open questions`
  section rather than being guessed at and buried in prose.
- **If you assume something, write the assumption down** in the same place the
  code that depends on it lives.
