/**
  ******************************************************************************
  * @file    control.h
  * @author  Angus Macdonald
  * @brief   Desired converter state, shared by every command source.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "pwm.h"

/* What the converter should be doing, separated from the act of making it so.
 *
 * Anything that can command the hardware - the serial link now, FDCAN and the
 * MPPT loop later - calls control_request_*() instead of touching the PWM
 * driver. Those calls validate, record and return immediately; they never
 * reconfigure a timer. control_service(), called once per pass of the main
 * loop, is the only code that drives pwm_*().
 *
 * Two things fall out of that. Several sources can command one channel without
 * racing each other inside the driver, and a burst of requests for the same
 * channel is applied as one batch inside a single stop/start bracket - so
 * "set 1 freq ..." followed by "set 1 duty ..." glitches a live output once
 * instead of twice.
 *
 * Actual state is not stored here. pwm_get_state() already folds latched
 * faults into the channel state and is the authority; this module holds only
 * what was *asked for*. */

typedef enum
{
  CONTROL_SRC_NONE = 0,
  CONTROL_SRC_SERIAL,
  CONTROL_SRC_CAN,
  CONTROL_SRC_MPPT
} control_source_t;

/* Clears every pending request. Call once from app_setup(). */
void control_init(void);

/* Record a request. Each returns false, and records nothing, when the value is
 * outside its config.h range - or, for control_request_run(true), when
 * pwm_get_state() already says the channel cannot start. That makes rejection
 * synchronous, so a transport can answer its host straight away rather than
 * waiting for the apply.
 *
 * Arbitration is last-writer-wins; `src` is recorded for diagnostics and is
 * the hook for a real policy once something other than the host writes here.
 *
 * Safe to call from interrupt context, though the intended FDCAN path buffers
 * frames and parses them from the loop instead. */
bool control_request_frequency(pwm_channel_id_t channel, uint32_t hz, control_source_t src);
bool control_request_duty(pwm_channel_id_t channel, uint16_t tenths, control_source_t src);
bool control_request_dead_time(pwm_channel_id_t channel, uint16_t ns, control_source_t src);
bool control_request_init(pwm_channel_id_t channel, control_source_t src);
bool control_request_run(pwm_channel_id_t channel, bool run, control_source_t src);
bool control_request_clear_ocp(pwm_channel_id_t channel, control_source_t src);
bool control_request_clear_ovp(control_source_t src);

/* Applies everything pending. Call once per pass, between the transports that
 * post requests and the reporters that describe the result. Returns true if
 * anything reached the hardware. */
bool control_service(void);

/* Diagnostics. `apply_failures` counts starts that were accepted but which
 * pwm_start() then refused - a fault that latched between request and apply. */
control_source_t control_last_source(pwm_channel_id_t channel);
uint32_t control_apply_failures(pwm_channel_id_t channel);

#endif /* CONTROL_H */
