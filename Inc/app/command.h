/**
  ******************************************************************************
  * @file    command.h
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
#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>

/* Protocol, host -> board. One command per line, terminated by CR, LF or both.
 * Verbs and channel names are case-insensitive; <ch> is a..e, 1..5 or "all".
 *
 *   set <ch> freq <hz>       PWM_MIN_FREQUENCY_HZ .. PWM_MAX_FREQUENCY_HZ
 *   set <ch> duty <tenths>   duty in tenths of a percent (500 = 50.0%)
 *   set <ch> dt <ns>         dead time, PWM_MIN .. PWM_MAX_DEAD_TIME_NS
 *   init <ch>                configure the timer and leave the channel stopped
 *   start <ch>               enable the outputs (fails unless STOPPED)
 *   stop <ch>                disable the outputs
 *   clear <ch>               clear that channel's latched OCP fault
 *   clear ovp                clear the global OVP fault
 *   get                      report the configuration of all five channels
 *
 * Values outside their range are rejected, not clamped - the host asked for a
 * specific number, and quietly substituting another one hides the difference
 * between "applied" and "nearly applied". The driver's own clamps stay as the
 * backstop for callers inside the firmware.
 *
 * Board -> host. Every reply line starts with '#', which keeps it clear of the
 * telemetry CSV (a host can tell the two apart on the first character):
 *
 *   #cfg,<ch>,<state>,<freq_hz>,<duty_tenths>,<dead_ns>
 *                            one per channel; <ch> is 1..5 and <state> is the
 *                            pwm_state_t value from pwm_get_state()
 *   #ok,<echo>               command accepted
 *   #err,<reason>,<echo>     command rejected, or - for start/clear against
 *                            "all" - at least one channel refused it
 *
 * The #cfg lines are the authority on what is in force; a reply only says how
 * the command itself was received. */

/* Drains complete lines from the transport, runs each one and queues its
 * reply. Call every pass of the main loop.
 *
 * This and command_report_service() must straddle the applier: the order
 *
 *     command_service();  control_service();  command_report_service();
 *
 * is what lets a command be parsed, applied and reported in the same pass, so
 * the "#cfg" lines that follow an "#ok" already describe the applied result.
 * Do not merge them into one call. */
void command_service(void);

/* Emits the periodic reports - the telemetry CSV on its own cadence, the
 * "#cfg" set on its, and the "#cfg" set immediately after any command. */
void command_report_service(void);

/* Runs one command line and writes its reply, CRLF-terminated, into out.
 * Returns the reply length in bytes, or 0 if the line held no command (blank
 * or comment) and nothing should be sent. Never writes more than size bytes. */
size_t command_execute(const char *line, char *out, size_t size);

/* Writes the "#cfg" line for one channel (0..PWM_CHANNEL_COUNT-1), CRLF
 * terminated. Returns its length, or 0 if the index is out of range. */
size_t command_format_config(uint32_t channel_index, char *out, size_t size);

/* Writes the telemetry CSV line, CRLF terminated. Returns its length, or 0 if
 * it did not fit. */
size_t command_format_telemetry(char *out, size_t size);

#endif /* COMMAND_H */
