/**
  ******************************************************************************
  * @file    dev_reporter.c
  * @author  Angus Macdonald
  * @brief   Bench print-out over UART5. Development only.
  ******************************************************************************
  */
#include "dev_reporter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "usart.h"

/* Longest line this will emit. Anything longer is dropped whole - see the
   header. 128 bytes at 115200 baud is 128 * 87 us = 11 ms, which is the worst
   case stall a single call can cost. */
#define DEV_LINE_MAX 128U

/* Generous: the transmit itself cannot take longer than DEV_LINE_MAX bytes at
   line rate, and it does not depend on anything being connected at the other
   end - a UART transmits into an empty room quite happily. This only exists
   so a wedged peripheral cannot hang the board forever. */
#define DEV_TX_TIMEOUT_MS 100U

/* Writes the line out and returns. Everything funnels through here, so this
   is the one place that touches the UART. */
static void dev_write(const char *text, size_t length) {
#if DEV_REPORTER_ENABLED
  if ((length == 0U) || (length > DEV_LINE_MAX)) {
    return;
  }

  (void)HAL_UART_Transmit(&huart5, (const uint8_t *)text, (uint16_t)length,
                          DEV_TX_TIMEOUT_MS);
#else
  (void)text;
  (void)length;
#endif
}

/* Builds "[<tick>] <body>\r\n" and sends it. A body that does not fit is
   dropped rather than cut short. */
static void dev_emit(const char *body) {
  char line[DEV_LINE_MAX];
  int written;

  written = snprintf(line, sizeof(line), "[%8lu] %s\r\n",
                     (unsigned long)HAL_GetTick(), body);

  /* snprintf returns the length it *would* have written, so a value at or
     past the buffer size means it did not fit. */
  if ((written > 0) && ((size_t)written < sizeof(line))) {
    dev_write(line, (size_t)written);
  }
}

/* Formats a float as an integer where it is one, and to three decimals where
   it is not - "520" rather than "520.000", which is most of what gets
   recorded. Written by hand because the project links --specs=nano.specs
   without -u _printf_float, so the library has no float conversions at all. */
static void dev_format_value(char *out, size_t size, float value) {
  const char *sign = "";
  uint32_t whole;
  uint32_t milli;

  /* NaN is the only value that compares unequal to itself. Catching it here
     keeps it out of the cast below, which would be undefined. */
  if (value != value) {
    (void)snprintf(out, size, "nan");
    return;
  }

  if (value < 0.0f) {
    sign = "-";
    value = -value;
  }

  /* Above this the cast to uint32_t is undefined, and a dev print is not
     worth a fault. 4.29e9 is the uint32_t ceiling. */
  if (value >= 4294967000.0f) {
    (void)snprintf(out, size, "%sbig", sign);
    return;
  }

  whole = (uint32_t)value;
  milli = (uint32_t)(((value - (float)whole) * 1000.0f) + 0.5f);

  /* Rounding can carry into the whole part: 1.9999 -> 1 and 1000. */
  if (milli >= 1000U) {
    whole++;
    milli -= 1000U;
  }

  if (milli == 0U) {
    (void)snprintf(out, size, "%s%lu", sign, (unsigned long)whole);
  } else {
    (void)snprintf(out, size, "%s%lu.%03lu", sign, (unsigned long)whole,
                   (unsigned long)milli);
  }
}

/* As dev_emit(), but with the tick supplied rather than read now - a replayed
   sample must carry the time it was captured. */
static void dev_emit_at(uint32_t tick_ms, const char *name, const char *value) {
  char line[DEV_LINE_MAX];
  int written;

  written = snprintf(line, sizeof(line), "[%8lu] %s=%s\r\n",
                     (unsigned long)tick_ms, name, value);

  if ((written > 0) && ((size_t)written < sizeof(line))) {
    dev_write(line, (size_t)written);
  }
}

/* One captured sample. The name is stored as a pointer, not a copy: callers
   pass string literals, which live in flash for the life of the program, so
   this costs 4 bytes per sample instead of a strcpy and a buffer. */
typedef struct {
  uint32_t tick_ms;
  const char *name;
  float value;
} dev_sample_t;

static dev_sample_t dev_samples[DEV_RECORD_MAX];
static uint32_t dev_count;
static uint32_t dev_drops;

void dev_record(const char *name, float value) {
#if DEV_REPORTER_ENABLED
  if (name == NULL) {
    return;
  }

  /* Full is not an error worth stopping the run for - the capture is simply
     shorter than asked. dev_flush() reports the count so it cannot pass for a
     run that ended early. */
  if (dev_count >= DEV_RECORD_MAX) {
    dev_drops++;
    return;
  }

  dev_samples[dev_count].tick_ms = HAL_GetTick();
  dev_samples[dev_count].name = name;
  dev_samples[dev_count].value = value;
  dev_count++;
#else
  (void)name;
  (void)value;
#endif
}

void dev_reset(void) {
  dev_count = 0U;
  dev_drops = 0U;
}

uint32_t dev_recorded(void) {
  return dev_count;
}

uint32_t dev_dropped(void) {
  return dev_drops;
}

void dev_flush(void) {
#if DEV_REPORTER_ENABLED
  uint32_t i;

  /* Summary lines carry no '=' on purpose: dev_monitor.py treats any
     name=value pair as data, and a "dropped=12" here would appear as a series
     on the plot. */
  dev_printf("# dev_flush start, %lu samples, %lu dropped",
             (unsigned long)dev_count, (unsigned long)dev_drops);

  for (i = 0U; i < dev_count; i++) {
    char value[24];

    dev_format_value(value, sizeof(value), dev_samples[i].value);

    /* Emitted with the tick recorded at capture time, not the tick now, so
       the timeline is the one the loop actually ran on. */
    dev_emit_at(dev_samples[i].tick_ms, dev_samples[i].name, value);
  }

  dev_printf("# dev_flush end");

  dev_reset();
#endif
}

void dev_printf(const char *fmt, ...) {
#if DEV_REPORTER_ENABLED
  char body[DEV_LINE_MAX];
  va_list args;
  int written;

  if (fmt == NULL) {
    return;
  }

  va_start(args, fmt);
  written = vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);

  if ((written > 0) && ((size_t)written < sizeof(body))) {
    dev_emit(body);
  }
#else
  (void)fmt;
#endif
}

void dev_print_i(const char *name, int32_t value) {
  dev_printf("%s=%ld", (name != NULL) ? name : "?", (long)value);
}

void dev_print_u(const char *name, uint32_t value) {
  dev_printf("%s=%lu", (name != NULL) ? name : "?", (unsigned long)value);
}

void dev_print_f(const char *name, float value) {
#if DEV_REPORTER_ENABLED
  char text[24];

  dev_format_value(text, sizeof(text), value);
  dev_printf("%s=%s", (name != NULL) ? name : "?", text);
#else
  (void)name;
  (void)value;
#endif
}

bool dev_every_ms(uint32_t *last_ms, uint32_t period_ms) {
#if DEV_REPORTER_ENABLED
  uint32_t now = HAL_GetTick();

  if (last_ms == NULL) {
    return false;
  }

  /* A zeroed static fires on the first call, which is what you want from a
     print. Unsigned subtraction, so this stays correct across the 32-bit tick
     wrap at ~49.7 days. */
  if ((*last_ms == 0U) || ((now - *last_ms) >= period_ms)) {
    *last_ms = now;
    return true;
  }

  return false;
#else
  (void)last_ms;
  (void)period_ms;
  return false;
#endif
}
