/**
  ******************************************************************************
  * @file    analog.h
  * @author  Angus Macdonald
  * @brief   Slow analog inputs on ADC1: bus voltage and the five NTCs.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef ANALOG_H
#define ANALOG_H

#include <stdint.h>

#include "config.h"
#include "pwm.h"

/* The single owner of ADC1. Six inputs, all slow, all telemetry: V_BUS_DIV and
 * the five channel NTCs. Nothing else may configure or start ADC1 - two owners
 * would race over the rank 1 configuration, which is why this module absorbed
 * the old vbus.c rather than sitting alongside it.
 *
 * The split across ADC instances is not a choice. NTC_CH1 (PF12) and NTC_CH2
 * (PF11) reach ADC1 alone, so ADC1 carries the slow group; the inductor
 * current sensors reach only ADC2 (channel 1) and ADC3 (channels 2-5) and will
 * be sampled from an HRTIM trigger when a control loop needs them. See
 * .agents/hardware.md.
 *
 * Conversions are polled, not DMA'd or interrupt-driven. One sweep of all six
 * inputs costs ~27 us against a 10 ms cadence, and a 16-bit conversion is
 * under 6 us, so an interrupt would cost more in latency and code than it
 * saves. */

/* Reported by analog_ntc_decicelsius() when the reading falls outside the
 * lookup table. That means an open sensor (the pull-up takes the pin toward
 * 3V3, above the -40 degC entry) or a short (the pin sits near 0 V, below the
 * 150 degC entry), not merely a cold or hot channel. The value is outside the
 * part's own -40..150 degC range, so it can never collide with a real one. */
#define ANALOG_TEMP_INVALID ((int16_t)-999)

/* Calibrates ADC1 and starts the cadence. Call once from app_setup(), after
 * MX_ADC1_Init(). */
void analog_init(void);

/* Non-blocking: every ANALOG_PERIOD_MS, converts all six inputs back to back
 * and returns. Between cadences it returns immediately. The whole set comes
 * from one sweep, so the bus voltage and the temperatures are always coherent
 * with each other. */
void analog_service(void);

/* Latest bus voltage in millivolts, with the divider undone. Zero until the
 * first sweep. */
uint32_t analog_vbus_mv(void);

/* Latest NTC temperature in tenths of a degree C (253 is 25.3 degC), or
 * ANALOG_TEMP_INVALID. Also ANALOG_TEMP_INVALID for an out-of-range channel
 * id, and until the first sweep completes. */
int16_t analog_ntc_decicelsius(pwm_channel_id_t channel);

/* Voltage at the NTC pin in millivolts, before any conversion - what a meter
 * on the pin should read. This is the measurement that separates a sensor
 * fault from a lookup table built for the wrong divider: the table converts
 * pin voltage to temperature and knows nothing about which resistors produced
 * it, so if the firmware and a meter agree here, any remaining error is in the
 * divider the table assumes. Reported by the "adc" host command. */
uint32_t analog_ntc_pin_mv(pwm_channel_id_t channel);

/* Voltage at the V_BUS_DIV pin in millivolts, before the divider is undone.
 * The control for the above: it shares the ADC and the code path, so if this
 * one is right the ADC is not the problem. */
uint32_t analog_vbus_pin_mv(void);

/* Self-check: the internal voltage reference, measured once at startup on
 * ADC3, against the value ST measured in the factory at VDDA = 3.3 V.
 *
 * This is the one reading with no external circuit behind it - no pin, no
 * divider, no source impedance, no sensor. If the measured raw value matches
 * the factory one, the ADC is converting correctly and any error is in what
 * the firmware assumes about the circuit. If it does not, nothing the ADC
 * reports can be trusted and the divider is a red herring.
 *
 * Both are raw 16-bit counts, deliberately - comparing raw to raw needs no
 * arithmetic that could itself be wrong. Expect them within a few percent.
 * VREFINT lives on ADC3 only, hence the different instance. */
uint16_t analog_vrefint_measured(void);
uint16_t analog_vrefint_factory(void);

/* Cross-check: NTC_CH5 (PA7) read through ADC2 instead of ADC1, in millivolts.
 *
 * PA7 is one of the pins wired to `INP7` on both instances, so two independent
 * converters can sample the same pad. VREFINT above proves ADC3 converts
 * correctly, which is *not* the same as proving ADC1 does - VREFINT is an
 * ADC3-only channel on this part. This closes that gap:
 *
 *   agrees with analog_ntc_pin_mv(PWM_CHANNEL_E)  -> the pad really is at that
 *                                                    voltage; look at the board
 *   disagrees                                     -> one converter is lying,
 *                                                    and ADC1 is the suspect
 *
 * Sampled once, during analog_init(), and not refreshed - iind.c owns ADC2 at
 * runtime and keeps it converting off an HRTIM trigger, so a polled read here
 * would be a second owner of the same peripheral. The pin voltage is static,
 * so a startup reading carries the same information. */
uint32_t analog_ntc5_via_adc2_mv(void);

#endif /* ANALOG_H */
