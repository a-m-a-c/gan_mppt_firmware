# Workflow and conventions

How this project is built, flashed, tested, and written.

---

## Toolchain

STM32CubeCLT 1.22.0 at `C:\ST\STM32CubeCLT_1.22.0`, deliberately **not on the
system PATH** — it would conflict with other toolchains on this machine.
VS Code's CMake Tools gets the paths injected via `cmake.configureEnvironment`
and `cmake.buildEnvironment` in `.vscode/settings.json`.

**Any command run outside VS Code must prepend the CLT bin directories itself:**

```
C:\ST\STM32CubeCLT_1.22.0\CMake\bin
C:\ST\STM32CubeCLT_1.22.0\Ninja\bin
C:\ST\STM32CubeCLT_1.22.0\GNU-tools-for-STM32\bin
C:\ST\STM32CubeCLT_1.22.0\STM32CubeProgrammer\bin
```

## Build

CMake presets, Ninja generator, output in `build/<preset>/`.

| Preset | |
|---|---|
| `Debug` | the one normally used |
| `Release` | |

In VS Code: **Ctrl+Shift+B** runs the default task `Build & Flash` (`CMake:
build` → `Flash (ST-LINK)`), no debug session. **F5** launches the debugger
instead.

## Flash

ST-LINK over SWD, via STM32CubeProgrammer CLI:

```
STM32_Programmer_CLI.exe -c port=SWD -w build/Debug/gan_mppt_firmware.elf -v -rst
```

`-rst` means the board starts running immediately.

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

**There is no automated test suite. Verification is manual, on hardware, over
the serial link.** An agent cannot verify a change here — it can only get it to
compile. Say which of the two you did.

### Host tools

Python tooling runs under **`uv`, never `pip`**. Single-file scripts carry PEP
723 inline dependency metadata, so `uv run` is the whole setup — no venv, no
`requirements.txt`, no `pyproject.toml`.

```
uv run tools/telem_gui.py          # web GUI: live plots + PWM control
uv run tools/stream_telem.py       # terminal CSV stream, quick sanity check
uv run tools/stream_telem.py --adc # + raw ADC pin voltages and self-checks
uv run tools/telem_gui.py --list   # show candidate serial ports
uv run tools/gen_ntc_table.py      # regenerate Inc/drivers/ntc_table.h
```

`telem_gui.py` is the primary bench tool: it plots any selection of telemetry
series over a chosen span, shows every field as a live number in the readout
grid below the plot, and sends PWM commands back over the same link. Readout
boxes double as the series toggles. It drives a live power stage — nothing is
energised until Start is pressed, but the duty slider applies as it moves, on a
running channel included.

The **inductor sensing panel** starts and stops sampling, sets the sample point
and recalibrates the zero. It reports **samples per second, not a total** —
a state of `running` with a rate of `0/s` means the HRTIM trigger is not
firing, which every other indicator on the page would show as healthy. The
calibrated zero counts are printed beneath it, because every current on the
plot is measured against them.

**The plot has two axes, left and right**, claimed by whichever units are
selected, in series order. Volts, amps and degrees at once is one unit too
many; the odd one out is named in the status line rather than silently not
drawn. The readout grid always shows every field regardless.

### The loop

1. Build and flash (Ctrl+Shift+B).
2. `uv run tools/telem_gui.py` — confirm telemetry is streaming and `#cfg` lines
   show the expected configuration.
3. Exercise the change. **Scope first, power second** for anything that alters
   switching behaviour.
4. Record anything learned about the board in [hardware.md](hardware.md), and
   anything decided in [decisions.md](decisions.md).

### Serial protocol

UART5, 115200 baud. One command per line (CR, LF or both). Channels are `a`–`e`,
`1`–`5` or `all`; verbs are case-insensitive.

```
set <ch> freq <hz>       set <ch> duty <tenths>     set <ch> dt <ns>
init <ch>                start <ch>                 stop <ch>
clear <ch>               clear ovp                  get
adc                      iind                       iind start | stop
iind zero                iind point <tenths>
```

Out-of-range values are **rejected, not clamped** — quietly substituting a
different number hides the difference between "applied" and "nearly applied".
The driver's own clamps are the backstop for internal callers.

Board → host: everything starting with `#` is a reply or report (`#cfg,...` /
`#iind,...` / `#ok,...` / `#err,...` / `#adc,...`); everything else is the
28-field telemetry CSV — `tick_ms, valid_mask, vbus_mv`, then five groups of
`vin_mv, iin_ma, vout_mv, iout_ma, iind_ma`. `valid_mask` bits 0..4 flag I2C
current data per channel, bits 5..9 flag live inductor sampling. `#cfg` and
`#iind` ride the same 1 s cadence and are the authority on what is in force.
Full grammar in [command.h](../Inc/app/command.h).

**Temperature is not reported.** `analog.c` still samples and converts the
NTCs, but the divider on the board is not understood — see
[decisions.md](decisions.md) 027 — so nothing consumes it. The `adc` command
still reports raw ADC pin voltages, VREFINT against its factory constant, and
an ADC1/ADC2 cross-check; `stream_telem.py --adc` polls it.

---

## Code layout

```
Inc/config.h        every tunable number, no includes/types/logic
Inc/drivers/  Src/drivers/    hardware-facing modules
Inc/app/      Src/app/        control and high-level logic
```

Generated CubeMX code is a thin init layer. Everything the project actually does
hangs off `app_setup()` and `app_loop()` in `Src/app/app.c`.

- **New sources** go in the top-level `CMakeLists.txt` at the
  `# Add user sources here` line in `target_sources()`; include paths at
  `# Add user defined include paths`.
- **Init calls** go in `USER CODE BEGIN 2` in `main.c` — in practice, inside
  `app_setup()` instead.
- **Never edit generated code outside `USER CODE` markers.** The `.ioc` is still
  actively edited and regenerated, and anything outside the markers is silently
  clobbered. One regeneration on 2026-07-25 deleted `main()` *including* its
  USER CODE blocks — **commit before regenerating.**

---

## Coding conventions

Derived from the existing code. Match it; do not introduce a second style.

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
- Public setters are the single path to hardware; `init` composes them rather
  than duplicating their register writes.

### Interrupts

`Src/drivers/interrupts.c` holds **every** HAL weak-callback override in the
project, so all interrupt entry points are visible in one place and modules
never collide over the shared callback symbols. Callbacks contain **routing
only** — the logic lives in a function in the owning module.

### HAL vs registers

**Use the HAL by default.** Drop to `SET_BIT`/`CLEAR_BIT` only where it is
genuinely necessary, and flag it in a comment. Established legitimate reasons:

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

The distinctive thing about this codebase, and worth preserving: **comments
explain why, and quantify.** They carry the arithmetic that justifies a number,
the failure mode a guard exists to prevent, and the constraint a caller must
respect. See the header of `Inc/config.h` or `Inc/app/control.h` for the
standard.

Do not write comments that restate the code. Do not strip the existing ones.

### Non-blocking

`app_loop()` must not block. Every service inside it is written to be called
often and return immediately — I2C transfers are sequenced across passes, ADC
conversions are sub-microsecond one-shots, serial is ring-buffered. Anything new
in the loop follows the same rule.

### Safety

Anything that can raise a duty cycle is safety-critical. Limits and ramps live
in the abstraction layer (`pwm.c`, `interrupts.c`), not in callers, so there is
one place to audit. See [hardware.md](hardware.md).
