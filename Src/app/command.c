/**
  ******************************************************************************
  * @file    command.c
  * @author  Angus Macdonald
  * @brief   Host command protocol (PWM configuration over the telemetry link).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "command.h"

#include <stdbool.h>
#include <stdio.h>

#include "analog.h"
#include "channel_telem.h"
#include "iind.h"
#include "control.h"
#include "main.h"
#include "pwm.h"
#include "serial.h"

/* The longest command is "set <ch> <param> <value>". Extra tokens are still
   counted (tokenize() returns the true total), so a longer line is rejected
   rather than acted on in truncated form. */
#define COMMAND_MAX_TOKENS 4U

/* Commands are echoed back so the host can match a reply to what it sent. A
   long line is truncated rather than rejected - the echo is for the operator,
   not for control flow. */
#define COMMAND_ECHO_MAX 48

#define COMMAND_ALL_CHANNELS ((1U << (uint32_t)PWM_CHANNEL_COUNT) - 1U)

/* Reply buffer: "#err,<reason>," plus a 47-char echo and CRLF fits in 96. */
#define COMMAND_REPLY_MAX 96

/* "#cfg,5,3,800000,900,300\r\n" is 25 bytes; 32 leaves room to add a field. */
#define COMMAND_CFG_MAX      32
#define COMMAND_CFG_SET_MAX  (COMMAND_CFG_MAX * (uint32_t)PWM_CHANNEL_COUNT)

/* Worst telemetry line: 10-char tick, mask, 5-char vbus, then 20 fields that
   can each reach "-102400" (read_reg20 sign-extends to +/-102.4 V), plus CRLF
   = 181 bytes. 224 leaves headroom without being wasteful. */
#define COMMAND_TELEM_MAX 256

/* A command is consumed only when there is room for everything answering it
   could produce: its reply, the config set it triggers, and the telemetry line
   that may fall due in the same pass. Without this reservation a burst of
   short bad commands amplifies - 2-byte junk yields 19-byte "#err" replies -
   and no ring size is big enough. With it, the RX ring becomes the elastic
   buffer and a genuine flood gets the honest "#err,rxlost" instead. */
#define COMMAND_TX_RESERVE (COMMAND_REPLY_MAX + COMMAND_CFG_SET_MAX + COMMAND_TELEM_MAX)

typedef struct
{
  const char *text;
  size_t length;
} token_t;

static bool is_space(char c)
{
  return (c == ' ') || (c == '\t');
}

static char lower(char c)
{
  return ((c >= 'A') && (c <= 'Z')) ? (char)(c + ('a' - 'A')) : c;
}

/* Splits on runs of whitespace. Returns the total token count, which may
   exceed max - only the first max are stored, and a caller that sees a larger
   count rejects the line rather than acting on a truncated command. */
static size_t tokenize(const char *line, token_t *tokens, size_t max)
{
  size_t count = 0U;
  size_t i = 0U;

  for (;;)
  {
    while (is_space(line[i]))
    {
      i++;
    }
    if (line[i] == '\0')
    {
      break;
    }

    size_t start = i;
    while ((line[i] != '\0') && !is_space(line[i]))
    {
      i++;
    }
    if (count < max)
    {
      tokens[count].text = &line[start];
      tokens[count].length = i - start;
    }
    count++;
  }

  return count;
}

/* Case-insensitive whole-token compare. `word` must be lower case. */
static bool token_is(const token_t *token, const char *word)
{
  size_t i;

  for (i = 0U; i < token->length; i++)
  {
    if ((word[i] == '\0') || (lower(token->text[i]) != word[i]))
    {
      return false;
    }
  }

  return word[i] == '\0';
}

/* Decimal digits only - no sign, no whitespace, and rejected on overflow
   rather than wrapped. */
static bool token_to_u32(const token_t *token, uint32_t *out)
{
  uint32_t value = 0U;

  if (token->length == 0U)
  {
    return false;
  }

  for (size_t i = 0U; i < token->length; i++)
  {
    char c = token->text[i];
    if ((c < '0') || (c > '9'))
    {
      return false;
    }
    uint32_t digit = (uint32_t)(c - '0');
    if (value > ((0xFFFFFFFFU - digit) / 10U))
    {
      return false;
    }
    value = (value * 10U) + digit;
  }

  *out = value;
  return true;
}

/* "a".."e", "1".."5" or "all", into a bitmask of channel indices. */
static bool parse_channels(const token_t *token, uint32_t *mask)
{
  if (token_is(token, "all"))
  {
    *mask = COMMAND_ALL_CHANNELS;
    return true;
  }
  if (token->length != 1U)
  {
    return false;
  }

  char c = lower(token->text[0]);
  if ((c >= 'a') && (c < (char)('a' + (int)PWM_CHANNEL_COUNT)))
  {
    *mask = 1U << (uint32_t)(c - 'a');
    return true;
  }
  if ((c >= '1') && (c < (char)('1' + (int)PWM_CHANNEL_COUNT)))
  {
    *mask = 1U << (uint32_t)(c - '1');
    return true;
  }

  return false;
}

/* Printable ASCII only, and no commas - the echo is the last field of a
   comma-separated reply, and line noise must not reshape it. */
static void sanitize(const char *line, char *out, size_t size)
{
  size_t out_index = 0U;

  for (size_t i = 0U; (line[i] != '\0') && (out_index < (size - 1U)); i++)
  {
    char c = line[i];
    if (c == ',')
    {
      c = ';';
    }
    else if ((c < 0x20) || (c > 0x7E))
    {
      c = '?';
    }
    out[out_index] = c;
    out_index++;
  }

  out[out_index] = '\0';
}

/* A reply that did not fit is dropped rather than sent truncated - a partial
   line would look like a different reply to the host. */
static size_t reply_length(int written, size_t size)
{
  return ((written > 0) && ((size_t)written < size)) ? (size_t)written : 0U;
}

static size_t reply_ok(char *out, size_t size, const char *echo)
{
  return reply_length(snprintf(out, size, "#ok,%s\r\n", echo), size);
}

static size_t reply_err(char *out, size_t size, const char *reason, const char *echo)
{
  return reply_length(snprintf(out, size, "#err,%s,%s\r\n", reason, echo), size);
}

/* Raw pin voltages, straight off the ADC with nothing applied - no divider
   undone, no lookup table consulted. Put a meter on the pin and compare: if
   they agree, the ADC is fine and any remaining error is in what the firmware
   assumes about the circuit feeding it. */
/* Inductor sensing status: what it is doing, where in the period it samples,
   how many sets have landed, and the calibrated zero per channel. The zeros
   are here rather than in the telemetry CSV because they change only when
   iind_calibrate_zero() runs, and every current on the CSV is measured against
   them - so a reading that looks wrong is checked here first. */
static size_t reply_iind(char *out, size_t size)
{
  return reply_length(snprintf(out, size, "#iind,%u,%u,%lu,%u,%u,%u,%u,%u\r\n",
                               (unsigned)iind_state(PWM_CHANNEL_A),
                               (unsigned)iind_sample_point(),
                               (unsigned long)iind_sample_id(),
                               (unsigned)iind_zero(PWM_CHANNEL_A),
                               (unsigned)iind_zero(PWM_CHANNEL_B),
                               (unsigned)iind_zero(PWM_CHANNEL_C),
                               (unsigned)iind_zero(PWM_CHANNEL_D),
                               (unsigned)iind_zero(PWM_CHANNEL_E)),
                      size);
}

/* iind start | stop | zero | point <tenths> | (bare) report
 *
 * "zero" is refused while any channel is running - see iind_calibrate_zero().
 * That refusal is surfaced as an #err rather than swallowed, because a zero
 * that silently did not happen leaves every later current wrong by the
 * operating point. */
static size_t handle_iind(const token_t *tokens, size_t count, char *out, size_t size,
                          const char *echo)
{
  uint32_t value;

  if (count == 1U)
  {
    return reply_iind(out, size);
  }

  if (token_is(&tokens[1], "start"))
  {
    return (count == 2U) ? (iind_start() ? reply_ok(out, size, echo)
                                         : reply_err(out, size, "refused", echo))
                         : reply_err(out, size, "usage", echo);
  }
  if (token_is(&tokens[1], "stop"))
  {
    if (count != 2U)
    {
      return reply_err(out, size, "usage", echo);
    }
    iind_stop();
    return reply_ok(out, size, echo);
  }
  if (token_is(&tokens[1], "zero"))
  {
    return (count == 2U) ? (iind_calibrate_zero() ? reply_ok(out, size, echo)
                                                  : reply_err(out, size, "running", echo))
                         : reply_err(out, size, "usage", echo);
  }
  if (token_is(&tokens[1], "point"))
  {
    if ((count != 3U) || !token_to_u32(&tokens[2], &value) || (value > UINT16_MAX))
    {
      return reply_err(out, size, "usage", echo);
    }
    return iind_set_sample_point((uint16_t)value) ? reply_ok(out, size, echo)
                                                  : reply_err(out, size, "range", echo);
  }

  return reply_err(out, size, "usage", echo);
}

static size_t reply_adc(char *out, size_t size)
{
  return reply_length(snprintf(out, size, "#adc,%lu,%lu,%lu,%lu,%lu,%lu,%u,%u,%lu\r\n",
                               (unsigned long)analog_vbus_pin_mv(),
                               (unsigned long)analog_ntc_pin_mv(PWM_CHANNEL_A),
                               (unsigned long)analog_ntc_pin_mv(PWM_CHANNEL_B),
                               (unsigned long)analog_ntc_pin_mv(PWM_CHANNEL_C),
                               (unsigned long)analog_ntc_pin_mv(PWM_CHANNEL_D),
                               (unsigned long)analog_ntc_pin_mv(PWM_CHANNEL_E),
                               (unsigned)analog_vrefint_measured(),
                               (unsigned)analog_vrefint_factory(),
                               (unsigned long)analog_ntc5_via_adc2_mv()),
                      size);
}

/* Posts one setpoint across every channel in the mask. The value has already
   been range-checked against the same config.h limits control.c uses, so these
   cannot fail. Nothing reaches the timer here - control_service() applies the
   whole batch later in this same pass. */
static void apply_set(uint32_t mask, const token_t *parameter, uint32_t value)
{
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    pwm_channel_id_t channel = (pwm_channel_id_t)i;

    if ((mask & (1U << i)) == 0U)
    {
      continue;
    }

    if (token_is(parameter, "freq"))
    {
      (void)control_request_frequency(channel, value, CONTROL_SRC_SERIAL);
    }
    else if (token_is(parameter, "duty"))
    {
      (void)control_request_duty(channel, (uint16_t)value, CONTROL_SRC_SERIAL);
    }
    else
    {
      (void)control_request_dead_time(channel, (uint16_t)value, CONTROL_SRC_SERIAL);
    }
  }
}

static size_t handle_set(const token_t *tokens, size_t count, char *out, size_t size,
                         const char *echo)
{
  uint32_t mask = 0U;
  uint32_t value = 0U;
  uint32_t minimum;
  uint32_t maximum;

  if (count != 4U)
  {
    return reply_err(out, size, "usage", echo);
  }
  if (!parse_channels(&tokens[1], &mask))
  {
    return reply_err(out, size, "badchannel", echo);
  }
  if (!token_to_u32(&tokens[3], &value))
  {
    return reply_err(out, size, "badvalue", echo);
  }

  if (token_is(&tokens[2], "freq"))
  {
    minimum = PWM_MIN_FREQUENCY_HZ;
    maximum = PWM_MAX_FREQUENCY_HZ;
  }
  else if (token_is(&tokens[2], "duty"))
  {
    minimum = PWM_MIN_DUTY_CYCLE;
    maximum = PWM_MAX_DUTY_CYCLE;
  }
  else if (token_is(&tokens[2], "dt"))
  {
    minimum = PWM_MIN_DEAD_TIME_NS;
    maximum = PWM_MAX_DEAD_TIME_NS;
  }
  else
  {
    return reply_err(out, size, "badparam", echo);
  }

  /* Checked here rather than left to the driver's clamp: the host asked for a
     specific value, and silently substituting another one is worse than
     saying no. It also keeps the uint16_t casts in apply_set() safe. */
  if ((value < minimum) || (value > maximum))
  {
    return reply_err(out, size, "range", echo);
  }

  apply_set(mask, &tokens[2], value);
  return reply_ok(out, size, echo);
}

/* init/start/stop take no value, so they share one shape: walk the mask and
   report whether every channel accepted. "start" is the only one that can be
   refused, and control_request_run() answers that from pwm_get_state() without
   waiting for the apply - so this reply means the same as it always did. */
static size_t handle_channel_verb(const token_t *tokens, size_t count, char *out, size_t size,
                                  const char *echo)
{
  uint32_t mask = 0U;
  bool refused = false;

  if (count != 2U)
  {
    return reply_err(out, size, "usage", echo);
  }
  if (!parse_channels(&tokens[1], &mask))
  {
    return reply_err(out, size, "badchannel", echo);
  }

  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    pwm_channel_id_t channel = (pwm_channel_id_t)i;

    if ((mask & (1U << i)) == 0U)
    {
      continue;
    }

    if (token_is(&tokens[0], "init"))
    {
      refused = !control_request_init(channel, CONTROL_SRC_SERIAL) || refused;
    }
    else if (token_is(&tokens[0], "start"))
    {
      refused = !control_request_run(channel, true, CONTROL_SRC_SERIAL) || refused;
    }
    else
    {
      refused = !control_request_run(channel, false, CONTROL_SRC_SERIAL) || refused;
    }
  }

  return refused ? reply_err(out, size, "refused", echo) : reply_ok(out, size, echo);
}

static size_t handle_clear(const token_t *tokens, size_t count, char *out, size_t size,
                           const char *echo)
{
  uint32_t mask = 0U;
  bool refused = false;

  if (count != 2U)
  {
    return reply_err(out, size, "usage", echo);
  }

  if (token_is(&tokens[1], "ovp"))
  {
    return control_request_clear_ovp(CONTROL_SRC_SERIAL)
               ? reply_ok(out, size, echo)
               : reply_err(out, size, "refused", echo);
  }
  if (!parse_channels(&tokens[1], &mask))
  {
    return reply_err(out, size, "badchannel", echo);
  }

  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    pwm_channel_id_t channel = (pwm_channel_id_t)i;

    if ((mask & (1U << i)) != 0U)
    {
      refused = !control_request_clear_ocp(channel, CONTROL_SRC_SERIAL) || refused;
    }
  }

  return refused ? reply_err(out, size, "refused", echo) : reply_ok(out, size, echo);
}

size_t command_execute(const char *line, char *out, size_t size)
{
  token_t tokens[COMMAND_MAX_TOKENS];
  char echo[COMMAND_ECHO_MAX];
  size_t count;

  if ((line == NULL) || (out == NULL) || (size == 0U))
  {
    return 0U;
  }

  count = tokenize(line, tokens, COMMAND_MAX_TOKENS);
  if (count == 0U)
  {
    return 0U; /* blank line - the host is idling, not asking for anything */
  }

  sanitize(line, echo, sizeof(echo));

  if (count > COMMAND_MAX_TOKENS)
  {
    return reply_err(out, size, "usage", echo);
  }

  if (token_is(&tokens[0], "set"))
  {
    return handle_set(tokens, count, out, size, echo);
  }
  if (token_is(&tokens[0], "init") || token_is(&tokens[0], "start") ||
      token_is(&tokens[0], "stop"))
  {
    return handle_channel_verb(tokens, count, out, size, echo);
  }
  if (token_is(&tokens[0], "clear"))
  {
    return handle_clear(tokens, count, out, size, echo);
  }
  if (token_is(&tokens[0], "get"))
  {
    return (count == 1U) ? reply_ok(out, size, echo) : reply_err(out, size, "usage", echo);
  }
  if (token_is(&tokens[0], "adc"))
  {
    return (count == 1U) ? reply_adc(out, size) : reply_err(out, size, "usage", echo);
  }
  if (token_is(&tokens[0], "iind"))
  {
    return handle_iind(tokens, count, out, size, echo);
  }

  return reply_err(out, size, "badcommand", echo);
}

size_t command_format_config(uint32_t channel_index, char *out, size_t size)
{
  pwm_channel_t *channel;
  int written;

  if ((out == NULL) || (channel_index >= (uint32_t)PWM_CHANNEL_COUNT))
  {
    return 0U;
  }

  channel = pwm_channel((pwm_channel_id_t)channel_index);
  if (channel == NULL)
  {
    return 0U;
  }

  written = snprintf(out, size, "#cfg,%lu,%u,%lu,%u,%u\r\n",
                     (unsigned long)(channel_index + 1U),
                     (unsigned)pwm_get_state(channel),
                     (unsigned long)channel->frequency,
                     (unsigned)channel->duty_cycle,
                     (unsigned)channel->dead_time);

  return reply_length(written, size);
}

/* Appends ",<vin_mv>,<iin_ma>,<vout_mv>,<iout_ma>" for one channel. Emits the
   last known good sample when the channel is invalid - the mask tells the host
   which is which, so a dead sensor never breaks the field count. */
/* The INA228 pair and the inductor amplifier measure different things on
   different hardware, and either can be dead while the other reads fine -
   hence a separate mask bit for each, and an inductor current emitted whether
   or not the I2C sensors answered. */
static size_t append_channel(char *out, size_t size, const telem_channel_t *t)
{
  int written = snprintf(out, size, ",%ld,%ld,%ld,%ld,%ld",
                         (long)(t->latest.vin_v * 1000.0f),
                         (long)(t->latest.iin_a * 1000.0f),
                         (long)(t->latest.vout_v * 1000.0f),
                         (long)(t->latest.iout_a * 1000.0f),
                         (long)iind_current_ma(t->number));

  return reply_length(written, size);
}

size_t command_format_telemetry(char *out, size_t size)
{
  static const telem_channel_t *const channels[] = {&telem_a, &telem_b, &telem_c,
                                                    &telem_d, &telem_e};
  size_t offset;
  size_t written;
  uint32_t mask = 0U;

  if (out == NULL)
  {
    return 0U;
  }

  /* Bits 0..4 are the INA228 pairs, 5..9 the inductor sensors - see command.h.
     One loop so the two halves cannot drift apart. */
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    mask |= channels[i]->valid ? (1U << i) : 0U;

    if (iind_state((pwm_channel_id_t)i) == IIND_STATE_RUNNING)
    {
      mask |= 1U << ((uint32_t)PWM_CHANNEL_COUNT + i);
    }
  }

  offset = reply_length(snprintf(out, size, "%lu,%lu,%lu",
                                 (unsigned long)HAL_GetTick(),
                                 (unsigned long)mask,
                                 (unsigned long)analog_vbus_mv()),
                        size);
  if (offset == 0U)
  {
    return 0U;
  }

  /* Every append is checked rather than accumulated blindly: snprintf returns
     the length it *would* have written, so adding that unchecked would push
     offset past the buffer and underflow the remaining-size subtraction. */
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    written = append_channel(&out[offset], size - offset, channels[i]);
    if (written == 0U)
    {
      return 0U;
    }
    offset += written;
  }

  written = reply_length(snprintf(&out[offset], size - offset, "\r\n"), size - offset);
  if (written == 0U)
  {
    return 0U;
  }

  return offset + written;
}

/* Set by any command, so a burst costs one config report at the end of the
   drain rather than one apiece. */
static bool command_cfg_due;
static uint32_t command_last_cfg_ms;
static uint32_t command_last_telem_ms;

void command_service(void)
{
  char line[COMMAND_REPLY_MAX];
  char reply[COMMAND_REPLY_MAX];

  for (uint32_t i = 0U; i < COMMAND_MAX_LINES_PER_PASS; i++)
  {
    /* Do not consume input that cannot be answered - see COMMAND_TX_RESERVE. */
    if (serial_tx_free() < COMMAND_TX_RESERVE)
    {
      return;
    }

    switch (serial_read_line(line, sizeof(line)))
    {
      case SERIAL_LINE_OK:
        (void)serial_write(reply, command_execute(line, reply, sizeof(reply)));
        command_cfg_due = true;
        break;

      case SERIAL_LINE_OVERFLOW:
      {
        static const char too_long[] = "#err,toolong,\r\n";
        (void)serial_write(too_long, sizeof(too_long) - 1U);
        break;
      }

      case SERIAL_LINE_LOST:
      {
        static const char lost[] = "#err,rxlost,\r\n";
        (void)serial_write(lost, sizeof(lost) - 1U);
        break;
      }

      case SERIAL_LINE_NONE:
      default:
        return;
    }
  }
}

/* Emits one "#cfg" line per channel and restarts the report interval. */
static void report_config(void)
{
  char line[COMMAND_CFG_MAX];

  /* Reserve the whole set up front, then write the lines individually. A
     partial set is harmless - each line stands alone - but a partial line is
     not, and checking once avoids five separate ways to fail. */
  if (serial_tx_free() < COMMAND_CFG_SET_MAX)
  {
    return;
  }

  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    (void)serial_write(line, command_format_config(i, line, sizeof(line)));
  }

  /* The inductor-sensing line rides the same cadence as the config set: it is
     the same kind of thing - state the host did not ask for but needs to stay
     in step with - and sending it unprompted means a host that connects
     mid-run sees the sampler's state without having to poll for it. */
  {
    char status[COMMAND_REPLY_MAX];

    (void)serial_write(status, reply_iind(status, sizeof(status)));
  }

  command_cfg_due = false;
  command_last_cfg_ms = HAL_GetTick();
}

void command_report_service(void)
{
  uint32_t now = HAL_GetTick();

  if (command_cfg_due || ((now - command_last_cfg_ms) >= REPORT_CFG_PERIOD_MS))
  {
    report_config();
  }

  /* Unsigned subtraction, so this stays correct across the 32-bit tick wrap
     at ~49.7 days. */
  if ((now - command_last_telem_ms) >= REPORT_TELEM_PERIOD_MS)
  {
    char line[COMMAND_TELEM_MAX];

    command_last_telem_ms = now;

    (void)serial_write(line, command_format_telemetry(line, sizeof(line)));
  }
}
