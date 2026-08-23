/**
  ******************************************************************************
  * @file    dev_reporter.h
  * @author  Angus Macdonald
  * @brief   Bench recording and print-out over UART5. Development only.
  ******************************************************************************
  */
#ifndef DEV_REPORTER_H
#define DEV_REPORTER_H

#include <stdbool.h>
#include <stdint.h>

/* Bench instrumentation. Include this header anywhere, call a function, read
   it in `uv run tools/dev_monitor.py`. Nothing in the firmware calls into
   this module, nothing knows it exists, and deleting it breaks nothing.

   It is deliberately not part of the architecture in .agents/project_plan.md.
   Do not build on it - the real host link is the command and telemetry path
   over serial and FDCAN, and this is not a step towards it.

   There are two ways to use it, and they are for different jobs.

   RECORD, then FLUSH - for anything inside a control loop.

       dev_record("vbus_mv", (float)sys.vbus_mv);   // cheap: no UART
       ...
       dev_flush();                                 // at the end of the run

   dev_record() stamps the value with the current tick and returns. It does no
   formatting and touches no peripheral, so a 10 ms control loop can call it
   every pass without the timing changing underneath the thing being measured.
   dev_flush() then sends the lot in one go and empties the buffer.

   PRINT - for one-off markers outside the timed section.

       dev_printf("cv begin, vin=%lu", (unsigned long)sys.vbus_mv);

   These format and transmit immediately, and they BLOCK: at 115200 baud a
   byte costs 87 us, so a 35-character line stalls the main loop for 3 ms - no
   ADC service, no I2C stepping, no control pass. Fine once at the start of a
   run. Not fine inside one; that is what dev_record() is for.

   Set DEV_REPORTER_ENABLED to 0 to compile every call down to an immediate
   return, without touching the call sites. */
#define DEV_REPORTER_ENABLED 1

/* Capacity of the record buffer, in samples. One sample is a tick, a name
   pointer and a value: 12 bytes on this target, so 6144 samples is 72 KB of
   the 128 KB DTCMRAM. Four series recorded at 100 Hz fill it in

       6144 / (4 * 100) = 15.4 seconds

   which is the number to change if a run needs to be longer than that. Past
   capacity, recording stops and the overflow is counted and reported by
   dev_flush() - a truncated capture says so rather than looking like a run
   that ended early. */
#define DEV_RECORD_MAX 6144U

/* Stores one value against the current millisecond tick. Cheap enough for a
   control loop: no formatting, no UART, no blocking.

   `name` must outlive the flush - a string literal always does. Only the
   pointer is stored, so the same literal costs nothing per sample.

   Values are held as float, which represents integers exactly up to 2^24
   (16777216). Every number on this board is far below that. */
void dev_record(const char *name, float value);

/* Sends everything recorded, as "[<tick>] <name>=<value>" lines, then empties
   the buffer ready for the next run.

   IT BLOCKS, for as long as the data takes at line rate: a 35-character line
   is 3 ms, so 1200 samples is about 3.6 seconds during which the main loop
   does not run at all. Call it somewhere that does not matter - after the
   channel is stopped, at the end of a run - never mid-regulation. */
void dev_flush(void);

/* Empties the buffer without sending it. Call at the start of a run so a
   capture cannot inherit samples from the last one. */
void dev_reset(void);

/* How many samples are held, and how many were dropped for want of room.
   Cheap; safe to read at any time. */
uint32_t dev_recorded(void);
uint32_t dev_dropped(void);

/* ------------------------------------------------------------------------ */

/* Prints one line immediately, prefixed with the tick and terminated with
   CRLF. BLOCKS - see the note above.

   Standard printf conversions, EXCEPT %f, %e and %g. The project links
   --specs=nano.specs without -u _printf_float, so floating point conversions
   are absent from the library and print nothing at all. Use dev_print_f().

   Anything past 128 characters is dropped rather than truncated - half a line
   in a terminal is worse than no line. */
void dev_printf(const char *fmt, ...);

/* "<name>=<value>" convenience for the common case. */
void dev_print_i(const char *name, int32_t value);
void dev_print_u(const char *name, uint32_t value);

/* As above, to three decimal places, formatted by hand so it does not need
   the floating point printf that nano.specs leaves out. Values that cannot be
   represented print as "nan" or "big" rather than as a wrong number. */
void dev_print_f(const char *name, float value);

/* Rate gate, so a call inside a fast loop costs a comparison. Pass a static
   of your own; it holds the last time this site fired:

       static uint32_t last;
       if (dev_every_ms(&last, 100U)) dev_print_u("vbus", sys.vbus_mv);

   The first call always fires. Correct across the 32-bit tick wrap. */
bool dev_every_ms(uint32_t *last_ms, uint32_t period_ms);

#endif /* DEV_REPORTER_H */
