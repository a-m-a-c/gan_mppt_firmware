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
 *   adc                      report raw ADC pin voltages (diagnostic)
 *   iind                     report inductor-sensing status
 *   iind start | stop        arm or disarm the HRTIM-triggered sampling
 *   iind zero                recalibrate the zero-current offset; refused
 *                            unless every channel is stopped
 *   iind point <tenths>      where in the switching period to sample
 *
 * Values outside their range are rejected, not clamped - the host asked for a
 * specific number, and quietly substituting another one hides the difference
 * between "applied" and "nearly applied". The driver's own clamps stay as the
 * backstop for callers inside the firmware.
 *
 * Board -> host, telemetry. One CSV line of 28 integer fields per
 * REPORT_TELEM_PERIOD_MS, never prefixed with '#':
 *
 *   <tick_ms>,<valid_mask>,<vbus_mv>,
 *   then five groups of <vin_mv>,<iin_ma>,<vout_mv>,<iout_ma>,<iind_ma>
 *
 * <iind_ma> is inductor current, signed - a synchronous boost can carry it
 * either way. It is sampled at 100 kHz and reported at 20 Hz, so this is a
 * trend rather than a waveform; a control loop reads iind.h directly.
 *
 * <valid_mask> carries two independent five-bit fields, because the INA228
 * pair and the inductor amplifier are different hardware and either can fail
 * alone. Bits 0..4 say channel 1..5 has current data from I2C; bits 5..9 say
 * channel 1..5 has live inductor sampling.
 *
 * Temperature is deliberately absent. analog.c still samples the NTCs and
 * converts them - see .agents/hardware.md for why the readings are not yet
 * believable - but nothing here reports them until the divider is understood.
 * Reporting a number that is smoothly wrong is worse than reporting none.
 *
 * Board -> host, replies. Every reply line starts with '#', which keeps it
 * clear of the telemetry CSV (a host can tell the two apart on the first
 * character):
 *
 *   #cfg,<ch>,<state>,<freq_hz>,<duty_tenths>,<dead_ns>
 *                            one per channel; <ch> is 1..5 and <state> is the
 *                            pwm_state_t value from the channel's pwm.op_state
 *   #ok,<echo>               command accepted
 *   #err,<reason>,<echo>     command rejected, or - for start/clear against
 *                            "all" - at least one channel refused it
 *   #adc,<vbus_mv>,<ntc1_mv>..<ntc5_mv>,<vrefint_raw>,<vrefint_cal>
 *                            millivolts at each ADC pin, with nothing applied:
 *                            no divider undone, no lookup table consulted.
 *                            A meter on the pin should read the same. That
 *                            separates an ADC problem from a wrong assumption
 *                            about the circuit feeding it.
 *
 *   #iind,<state>,<point>,<sample_id>,<zero1>..<zero5>
 *                            inductor sensing status. <state> is iind_state_t,
 *                            <point> the sample position in tenths of a
 *                            percent, <sample_id> a count of completed sets,
 *                            and the five zeros the calibrated offsets in raw
 *                            counts. Every current on the CSV is measured
 *                            against those zeros, so a reading that looks
 *                            wrong is checked here first.
 *
 *                            The last two settle it without a meter at all:
 *                            the internal reference as measured at startup,
 *                            and the raw count ST measured in the factory at
 *                            VDDA = 3.3 V. No pin or divider is involved, so
 *                            if those two disagree the ADC itself is wrong
 *                            and every other number here is meaningless.
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

/* Writes the "#cfg" line for one channel (0..CHANNEL_COUNT-1), CRLF
 * terminated. Returns its length, or 0 if the index is out of range. */
size_t command_format_config(uint32_t channel_index, char *out, size_t size);

/* Writes the telemetry CSV line, CRLF terminated. Returns its length, or 0 if
 * it did not fit. */
size_t command_format_telemetry(char *out, size_t size);

#endif /* COMMAND_H */
