# .agents — orientation

**The code is the authority on what the firmware does, and
[todo.md](todo.md) is the authority on what is outstanding.** These files carry
what those two cannot: board facts, the working rules and the build loop. They
are written to be read by more than one model (Claude Opus and GPT are both
used here), so nothing lives in a model's private memory.

| File | What it is | Read it when |
|---|---|---|
| [project_agent_instructions.md](project_agent_instructions.md) | The author's instructions and the working agreement | **Always, first.** |
| This file | File index and the rules for keeping these files honest | Before adding to any of them |
| [hardware.md](hardware.md) | Board facts: converters, sensors, limits, pin/fault mapping, derived safety numbers | Before touching anything that drives or measures hardware |
| [workflow.md](workflow.md) | Build, flash, bench test loop, code layout, coding conventions | Before building, flashing or writing code |
| [todo.md](todo.md) | The project todo list, in the author's words | For what is outstanding. **Never implement from it unprompted.** |
| [thesis_discussion_points.md](thesis_discussion_points.md) | Critical revelations: decisions that changed, assumptions that broke | When something non-obvious turns up, or when writing about the project |

## Rules for maintaining these files

- **Facts go in [hardware.md](hardware.md).** Anything measurable about the
  board. If a number here disagrees with the code, one of them is a bug — say
  so rather than silently trusting either.
- **No architecture here.** How the modules fit together is read from the code,
  not from a diagram that goes stale the moment it is written. These files say
  what the code cannot say about itself. Where a doc and the code disagree
  about what exists, the code wins and the doc gets fixed.
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
- **Open questions stay visible.** Raise an unanswered question as a question
  rather than guessing at it and burying the guess in prose.
- **If you assume something, write the assumption down** in the same place the
  code that depends on it lives.
