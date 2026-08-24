# Workflow and conventions

How this project is built, flashed, tested, and written.

---

## Toolchain

STM32CubeCLT 1.22.0 at `C:\ST\STM32CubeCLT_1.22.0`, on the **machine** PATH.
Its `CMake\bin`, `Ninja\bin`, `Make\bin`, `GNU-tools-for-STM32\bin`,
`st-arm-clang\bin`, `STLink-gdb-server\bin`, and `STM32CubeProgrammer\bin` all
resolve in a bare shell — `cmake`, `ninja`, `arm-none-eabi-gcc`, `make`, and
`STM32_Programmer_CLI` need nothing prepended, inside VS Code or out.

`.vscode/settings.json` still injects the same paths via
`cmake.configureEnvironment` / `cmake.buildEnvironment`. That is redundant now
but deliberate: it pins CMake Tools to CubeCLT's copies no matter what else
lands on PATH later. Leave it.

**One shadowing conflict to know about.** Vivado
(`C:\Xilinx\2025.1\Vivado\bin`, user PATH) also ships a `ninja`. Machine PATH
is composed before user PATH, so CubeCLT's `ninja` wins everywhere. That is the
outcome this repo wants; if Vivado ever misbehaves outside its own
`settings64.bat`, this is why.

## Build and flash

CMake presets, Ninja generator, output in `build/<preset>/`. `Debug` is the one
normally used; `Release` exists.

In VS Code: **Ctrl+Shift+B** runs the default task `Build & Flash` (`CMake:
build` → `Flash (ST-LINK)`), no debug session. **F5** launches the debugger.

Flashing is ST-LINK over SWD, via the STM32CubeProgrammer CLI. `-rst` means the
board starts running immediately:

```
STM32_Programmer_CLI.exe -c port=SWD -w build/Debug/gan_mppt_firmware.elf -v -rst
```

### ST-LINK recovery

The probes here are homemade and occasionally need recovering. Two traps:

- **`stlinkserver` does not exit with STM32CubeIDE.** It is a detached broker
  and keeps holding the probe, so tools report `(in use)` / error 0x3. Kill it
  by PID.
- **`STLinkUpgrade.bat` fails silently on a blank probe.** It passes `-update`,
  which cannot auto-select an image on a blank device, and it runs `javaw` (no
  console) and deletes its log. Launch the GUI directly instead:

  ```
  & "C:\ST\STM32CubeCLT_1.22.0\jre\bin\javaw.exe" -jar "C:\ST\STM32CubeCLT_1.22.0\STLink-gdb-server\bin\STLinkUpgrade.jar"
  ```

Firmware images are bundled inside the jar — nothing to download. A probe
showing `Type`/`Version` as `Unknown` usually means bootloader present but no
application, not corruption.

---

## Testing

**There is no automated test suite. Verification is manual, on hardware.** An
agent cannot verify a change here — it can only get it to compile. Say which of
the two you did.

The loop:

1. Build and flash (Ctrl+Shift+B).
2. Confirm the board reaches STANDBY — `LED_ERR` dark, and `LED_OUT_CONN`
   tracking the bus.
3. Exercise the change. **Scope first, power second** for anything that alters
   switching behaviour.
4. Record anything learned about the board in [hardware.md](hardware.md).

> **The board still has no command link.** The serial command/telemetry module
> was removed in `efaf6bb` and nothing has replaced it, so the board cannot be
> told anything - a stub in `command.c` asks for CV a second after boot. What
> exists instead is one-way instrumentation, below.

### Bench instrumentation — `dev_reporter`

`Inc/dev/dev_reporter.h` is a development-only print-out over UART5, deliberately
outside the architecture in [project_plan.md](project_plan.md). Nothing in the
firmware calls it and deleting it breaks nothing. **Do not build on it** - the
real host link is the command and telemetry path, and this is not a step
towards it.

Two ways to use it, for different jobs:

- `dev_record()` / `dev_flush()` — store to RAM during a run, send it all at the
  end. Cheap enough for a control loop. Use this for anything timed.
- `dev_printf()` / `dev_print_*()` — format and send immediately. **Blocks**;
  at 115200 a 35-character line stalls the main loop for 3 ms. One-off markers
  only.

### Host tools

Python tooling runs under **`uv`, never `pip`**. Single-file scripts carry
PEP 723 inline dependency metadata, so `uv run` is the whole setup — no venv,
no `requirements.txt`, no `pyproject.toml`.

```
uv run tools/bench_run.py          # build, flash, capture, write serial_flush.svg
uv run tools/dev_monitor.py        # just watch the UART
uv run tools/dev_monitor.py --plot # capture and plot, then exit
uv run tools/dev_monitor.py --list # show candidate serial ports
uv run tools/gen_ntc_table.py      # regenerate an NTC lookup table
```

`bench_run.py` is the bench loop in one command — it runs the same CMake preset
Ctrl+Shift+B does, flashes with `-rst`, then hands over to `dev_monitor.py`.

`dev_monitor.py` prints lines and parses nothing beyond `name=value`. `--plot`
arms on the first data line, ends when the stream goes quiet, writes
`serial_flush.svg` in the repo root and exits — so a buffered `dev_flush()`
burst needs no duration guessed in advance. `-t` is only a safety timeout.

`telem_gui.py` and `stream_telem.py` were deleted: they spoke the removed
protocol. `git show efaf6bb^:Inc/app/command.h` is the record of that wire
format.

### Serial protocol — TODO, not implemented

UART5, 115200 baud, one command per line (CR, LF or both). Channels are `a`–`e`,
`1`–`5` or `all`; verbs case-insensitive. The grammar the removed module spoke,
in full, is `git show efaf6bb^:Inc/app/command.h`. In outline:

```
set <ch> freq <hz>       set <ch> duty <tenths>     set <ch> dt <ns>
init <ch>                start <ch>                 stop <ch>
clear <ch>               clear ovp                  get
```

Board → host: everything starting with `#` is a reply or report
(`#cfg,...` / `#ok,...` / `#err,...`); everything else is the telemetry CSV.

**Out-of-range values are rejected, not clamped.** Quietly substituting a
different number hides the difference between "applied" and "nearly applied".
The driver's own clamps are the backstop for internal callers.

When this is rebuilt it is **transport-agnostic** — FDCAN carries the same
message set — see [project_plan.md](project_plan.md).

---

## Code layout

The directory split and what belongs in each part is in
[project_plan.md](project_plan.md#firmware-architecture). The rules for adding
to it are here.

Generated CubeMX code is a thin init layer. Everything the project actually
does hangs off `app_setup()` and `app_loop()` in `Src/app/app.c`.

- **New sources** go in the top-level `CMakeLists.txt` at the
  `# Add user sources here` line in `target_sources()`; include paths at
  `# Add user defined include paths`.
- **Init calls** go inside `app_setup()`, not `main.c`.
- **Never edit generated code outside `USER CODE` markers.** The `.ioc` is
  still actively edited and regenerated, and anything outside the markers is
  silently clobbered. One regeneration on 2026-07-25 deleted `main()`
  *including* its USER CODE blocks — **commit before regenerating.**

---

## Coding conventions

Derived from the existing code. Match it; do not introduce a second style.

### Brace style

Opening brace on the same line, for functions and control statements alike.
`pwm.c` is the reference:

```c
void led_init(void) {
  if (running) {
    stop();
  } else {
    start();
  }
}
```

### Indentation

**Two spaces, no tabs.** `.vscode/settings.json` sets this workspace-wide with
`editor.detectIndentation` off — left on, VS Code infers the width from each
file's contents and silently overrides it. `tools/` is exempt back to 4, being
PEP 8.

### Naming and structure

- `snake_case` throughout. **Short module prefix on every public symbol**
  matching the file: `pwm_init()`, `pwm_set_duty_cycle()`, `telem_service()`.
  Short prefixes beat descriptive ones (`pwm_` was chosen over
  `timer_control_`).
- **Explicit per-channel code over arrays and loops.** There are exactly five
  channels, named A–E, and they are named in the code (`channel_a`, `telem_b`).
  Internal lookup tables are fine; the public surface is explicit.
- One module = one `.c`/`.h` pair with a doxygen banner header carrying
  `@file`, `@author`, `@brief` and the licence block. Include guards are bare
  `MODULE_H`.
- **Drivers publish into `sys` and `channel_x` and keep no second copy.** There
  is one home for a number.
- Public setters are the single path to hardware; `init` composes them rather
  than duplicating their register writes.

### Interrupts

`Src/drivers/interrupts.c` holds **every** HAL weak-callback override in the
project, so all interrupt entry points are visible in one place and modules
never collide over the shared callback symbols. Callbacks contain **routing
only** — the logic lives in a function in the owning module.

### HAL vs registers

**Use the HAL by default.** Drop to `SET_BIT`/`CLEAR_BIT` only where genuinely
necessary, and flag it in a comment. Established legitimate reasons:

- **Lock-sensitive / ISR-reachable paths.** Most HAL calls start with
  `__HAL_LOCK` and return `HAL_BUSY` doing nothing if the handle is already
  locked. A fault-stop that silently no-ops because the main loop held the lock
  is unacceptable, so fault paths write `ODISR`/`MCR`/`OENR` directly.
- **Write ordering the HAL does not expose** — e.g. forcing a preload transfer
  via `CR2` before enabling outputs, so the first edge uses the right duty.
- Short deterministic work inside `enter_critical()`/`exit_critical()`.

Everywhere else — peripheral init, one-shot config, non-ISR control — use the
HAL call.

### Comments

**Keep them short.** The code is self-documenting and the author reads code
well — a single line or two is usually plenty. Use `//`; reach for `/* */` only
when a comment genuinely runs long. This is an Agent Instruction in
[project_plan.md](project_plan.md), not a preference.

Spend that space on what the code cannot say: the arithmetic behind a number,
the failure a guard prevents, the constraint a caller must respect, what breaks
if two calls are reordered. Restating the statement below it is noise.

Do not strip the existing ones.

### Non-blocking

`app_loop()` must not block, and anything added to it follows the same rule.
Why, and what that costs each service, is in
[project_plan.md](project_plan.md#the-loop).

### Safety

Anything that can raise a duty cycle is safety-critical. Limits and ramps live
in the abstraction layer (`pwm.c`, `interrupts.c`), not in callers, so there is
one place to audit. See [hardware.md](hardware.md).
