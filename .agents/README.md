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
| [todo.md](todo.md) | The project todo list, in the author's words | For what is outstanding. **Never implement from it unprompted.** |
| [thesis_discussion_points.md](thesis_discussion_points.md) | Insights worth writing up: decisions that changed, assumptions that broke | When something non-obvious turns up, or when writing about the project |

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
- **Unfinished work goes in [todo.md](todo.md),** not in
  [project_plan.md](project_plan.md), which describes intent and architecture
  rather than a work queue. `TODO` comments still belong **in the code**, next
  to the line they concern - that is where they keep their context. Write
  todo.md entries in plain short sentences, and never work from it unprompted.
- **[thesis_discussion_points.md](thesis_discussion_points.md) is for insight,
  not progress.** Add an entry when something changed a design decision,
  contradicted an assumption, or is a general lesson rather than a project
  detail - and record the evidence, not just the conclusion. Routine work does
  not belong there.
- **Sections marked `DO NOT EDIT` are the author's words.** Do not reword,
  reformat or "improve" them. Propose changes in conversation instead.
- **Open questions stay visible.** Unanswered items live in an `Open questions`
  section rather than being guessed at and buried in prose.
- **If you assume something, write the assumption down** in the same place the
  code that depends on it lives.
