/**
  ******************************************************************************
  * @file    iind.h
  * @author  Angus Macdonald
  * @brief   Inductor current sensing (INA310 amplifiers, HRTIM-triggered ADC).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef IIND_H
#define IIND_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "pwm.h"

/* The five INA310 current-sense amplifiers across the 3 mOhm shunts, sampled
 * at a fixed point in the switching period. Named for the board's nets
 * (I_IND_1..5) so it cannot be confused with the INA228 input/output currents,
 * which measure something else entirely and answer far more slowly.
 *
 * Channels are addressed by pwm_channel_id_t, so a caller says
 * iind_current_ma(PWM_CHANNEL_A) and gets the current in the same channel it
 * commanded. That mapping is the point of this layer.
 *
 * WHY THE TRIGGER MATTERS MORE THAN THE RATE
 *
 * Inductor current is a triangle wave - about 0.83 A peak-to-peak on 8.22 A at
 * 500 kHz with 33 uH. Sampling at an arbitrary phase returns a random point on
 * that triangle, and no amount of oversampling removes it, because it is not
 * noise. Sampling at the same point every period returns a deterministic point,
 * and sampling at the midpoint of the on-time returns the average directly with
 * no filter at all. So this module is built around *when* it converts.
 *
 * HOW THE SAMPLE IS CLOCKED
 *
 * The HRTIM master timer - unused by pwm.c, which drives only timers A..E - is
 * run as the sample clock. Its period is a whole multiple of the switching
 * period, so the sample instant is phase-locked to the converter rather than
 * drifting through it, and its compare 1 places that instant within the period.
 * Master compare 1 raises HRTIM ADC trigger 1, which starts both ADCs at once.
 *
 * WHY TWO ADCs
 *
 * Not a choice. I_IND_1 reaches ADC2 alone and I_IND_2..5 reach ADC3 alone, so
 * no single sequence covers all five. Both are triggered from the same HRTIM
 * event, so channel A is sampled at the trigger instant and B..E follow in
 * sequence behind it. See .agents/hardware.md for the pin map. */

/* Effective per-channel state, reported by iind_state(). */
typedef enum
{
  IIND_STATE_UNINITIALIZED = 0,
  IIND_STATE_STOPPED,  /* configured, not converting */
  IIND_STATE_RUNNING,  /* converting, samples arriving */
  IIND_STATE_STALE     /* started, but no sample within IIND_STALE_TIMEOUT_MS */
} iind_state_t;

/* Configures both ADCs, the DMA streams and the master timer, and measures the
 * zero-current offset. Call once from app_setup(), after MX_ADC2_Init(),
 * MX_ADC3_Init() and MX_HRTIM_Init(), and before any channel is started.
 *
 * Leaves every channel STOPPED. Returns false if the ADCs are not configured
 * the way this module needs - which, until the CubeMX changes in
 * .agents/hardware.md are made, is what will happen. */
bool iind_init(void);

/* Zero-current offset, in raw counts, captured per channel.
 *
 * The amplifier's output at zero current is not assumed. It may sit at ground
 * for a unidirectional part or at a mid-rail reference for a bidirectional
 * one, and either way it carries the amplifier's own offset error. Measuring it
 * costs nothing and removes both.
 *
 * INVARIANT: this is only meaningful when no current is flowing, so it refuses
 * unless every channel reports PWM_STATE_STOPPED or PWM_STATE_UNINITIALIZED.
 * Calibrating against a live converter would fold the operating current into
 * the zero and silently bias every later reading toward it. Returns false if
 * refused; the previous offsets are then left untouched. */
bool iind_calibrate_zero(void);

/* Starts and stops the sample clock. Conversions are free-running once armed -
 * DMA writes the latest value into memory and nothing needs servicing per
 * sample, so there is no iind_service() in the main loop. */
bool iind_start(void);
void iind_stop(void);

/* Where in the switching period the sample is taken, in tenths of a percent of
 * the period (500 = halfway). The useful setting is the middle of a channel's
 * on-time, where the triangle crosses its own average.
 *
 * Rejected, with nothing changed, outside IIND_MIN/MAX_SAMPLE_POINT - the
 * extremes sit on a switching edge, where the amplifier is still slewing and
 * the reading is whatever the transient happened to be doing.
 *
 * KNOWN LIMITATION: the master timer is not explicitly phase-aligned to timers
 * A..E, so the offset between this setting and a channel's actual switching
 * edge is constant but not zero, and not known in advance. The setting is
 * therefore calibrated on a scope rather than trusted as an absolute position.
 * Aligning them properly needs the HRTIM's synchronisation or a timer reset
 * source, which is a change to how pwm.c starts its timers. */
bool iind_set_sample_point(uint16_t tenths);
uint16_t iind_sample_point(void);

/* Latest inductor current in milliamps, signed - negative means current is
 * flowing back out of the bus, which a synchronous boost can do. Zero for an
 * out-of-range channel id, and until the first sample arrives. */
int32_t iind_current_ma(pwm_channel_id_t channel);

/* The raw count and the calibrated zero behind that figure, for diagnosing a
 * reading that looks wrong without having to work backwards through the scale
 * factor. */
uint16_t iind_raw(pwm_channel_id_t channel);
uint16_t iind_zero(pwm_channel_id_t channel);

iind_state_t iind_state(pwm_channel_id_t channel);

/* Counts completed sample sets. A control loop watches this rather than a
 * timer, so it runs on fresh data instead of on a guess about the rate. */
uint32_t iind_sample_id(void);

/* ADC3 conversion-complete hook, called from the HAL callback in interrupts.c.
 * ADC3 carries four of the five channels, so its sequence completing is what
 * marks a full set. Not for application use. */
void iind_conversion_complete(void);

#endif /* IIND_H */
