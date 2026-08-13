/**
  ******************************************************************************
  * @file    led.h
  * @author  Angus Macdonald
  * @brief   Toggle LED heartbeat (LED_TOG_1..5 plus the three status LEDs).
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

/* Full on/off cycle. The lines sit high for half of this and low for the
 * other half, so the visible blink rate is 0.5 Hz. */
#define LED_TOGGLE_PERIOD_MS 2000U

/* Drives all eight lines low and starts the cadence. Call once from main().
 * Covers LED_TOG_1..5 plus LED_ACTIVE, LED_ERR and LED_OUT_CONN; the latter
 * three are flashed for bring-up and should be handed back to real status
 * logic once that exists. */
void led_init(void);

/* Non-blocking: toggles the lines when half a period has elapsed, otherwise
 * returns immediately. Must be called often enough that the caller does not
 * starve it - see led_delay_ms(). */
void led_toggle_service(void);

/* HAL_Delay() replacement that keeps the heartbeat running while it waits.
 * Use in place of HAL_Delay() anywhere in the main loop, otherwise the
 * blocking wait stalls the toggle. */
void led_delay_ms(uint32_t ms);

#endif /* LED_H */
