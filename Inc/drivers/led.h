/**
  ******************************************************************************
  * @file    led.h
  * @author  Angus Macdonald
  * @brief   Status LEDs driven from system state.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef LED_H
#define LED_H

#include <stdint.h>

#include "config.h"

/* Every line reflects state - nothing blinks any more:
 *
 *   LED_TOG_1..5   the matching PWM channel is RUNNING
 *   LED_ACTIVE     any channel is RUNNING
 *   LED_ERR        any OCP is latched, or the global OVP is
 *   LED_OUT_CONN   the bus is above LED_BUS_ON_MV (a battery is connected)
 *
 * Thresholds and drive polarity are in config.h. */

/* Drives every line to match the current state. Call once from main(). The
 * bus LED stays dark until the first telemetry sample lands, a cycle later. */
void led_init(void);

/* Non-blocking: refreshes all eight lines from system state and returns.
 * Cheap enough to call every pass of the main loop, which is what keeps a
 * fault visible the instant it latches. */
void led_service(void);

#endif /* LED_H */
