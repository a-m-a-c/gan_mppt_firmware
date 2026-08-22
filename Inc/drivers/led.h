/**
  ******************************************************************************
  * @file    led.h
  * @author  Angus Macdonald
  * @brief   Status LED pins.
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

#include <stdbool.h>
#include <stdint.h>
/* Pin writes and drive polarity, and nothing else. Each function takes "should
 * this LED be lit" and makes the pin match, so no caller ever needs to know
 * that the per-channel lines are active low and the status lines are active
 * high.
 *
 * Deciding *when* a light should be on is not this module's job - that reads
 * converter and bus state, and lives with the code that owns them.
 *
 * Channel LEDs are silkscreened LED_TOG_1..5 and map to channels A..E in
 * order. They are named by channel here because uint32_t is the identity
 * the rest of the firmware uses, and one numbering scheme is enough. */

/* Drives every line to its off state. Call once, before anything else can
 * light one. */
void led_init(void);

void led_set_channel_a(bool on);
void led_set_channel_b(bool on);
void led_set_channel_c(bool on);
void led_set_channel_d(bool on);
void led_set_channel_e(bool on);

void led_set_active(bool on);   /* LED_ACTIVE   */
void led_set_err(bool on);      /* LED_ERR      */
void led_set_out_conn(bool on); /* LED_OUT_CONN */

void led_channel_flash_begin(uint32_t interval_ms);
void led_channel_flash_service(void);

#endif /* LED_H */
