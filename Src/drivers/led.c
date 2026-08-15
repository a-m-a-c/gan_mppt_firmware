/**
  ******************************************************************************
  * @file    led.c
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
#include "led.h"

#include <stdbool.h>

#include "main.h"
#include "pwm.h"
#include "vbus.h"

/* Each line carries its own polarity: the channel lines and the status LEDs
   are driven by different circuits. */
typedef struct
{
  GPIO_TypeDef *port;
  uint16_t pin;
  bool active_high;
} led_line_t;

/* LED_TOG_n belongs to PWM channel n, in order. */
static const led_line_t led_channel_lines[PWM_CHANNEL_COUNT] = {
    [PWM_CHANNEL_A] = {LED_TOG_1_GPIO_Port, LED_TOG_1_Pin, LED_TOG_ACTIVE_HIGH},
    [PWM_CHANNEL_B] = {LED_TOG_2_GPIO_Port, LED_TOG_2_Pin, LED_TOG_ACTIVE_HIGH},
    [PWM_CHANNEL_C] = {LED_TOG_3_GPIO_Port, LED_TOG_3_Pin, LED_TOG_ACTIVE_HIGH},
    [PWM_CHANNEL_D] = {LED_TOG_4_GPIO_Port, LED_TOG_4_Pin, LED_TOG_ACTIVE_HIGH},
    [PWM_CHANNEL_E] = {LED_TOG_5_GPIO_Port, LED_TOG_5_Pin, LED_TOG_ACTIVE_HIGH},
};

static const led_line_t led_status_active = {LED_ACTIVE_GPIO_Port, LED_ACTIVE_Pin,
                                             LED_STATUS_ACTIVE_HIGH};
static const led_line_t led_status_error = {LED_ERR_GPIO_Port, LED_ERR_Pin,
                                            LED_STATUS_ACTIVE_HIGH};
static const led_line_t led_status_bus = {LED_OUT_CONN_GPIO_Port, LED_OUT_CONN_Pin,
                                          LED_STATUS_ACTIVE_HIGH};

/* The bus LED latches its own output so the two thresholds can straddle the
   present reading - see led_bus_connected(). */
static bool led_bus_lit;

static void led_write(const led_line_t *line, bool lit)
{
  /* Drive high when "lit" and "active high" agree, low when they differ. */
  bool high = (lit == line->active_high);

  HAL_GPIO_WritePin(line->port, line->pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Any latched fault, read from the flags rather than from pwm_get_state():
   a channel that faulted before it was ever initialised still reports
   UNINITIALIZED, and that fault should light the LED all the same. */
static bool led_any_fault(void)
{
  if (pwm_OVP_fault_latched)
  {
    return true;
  }

  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    const pwm_channel_t *channel = pwm_channel((pwm_channel_id_t)i);
    if ((channel != NULL) && channel->ocp_fault)
    {
      return true;
    }
  }

  return false;
}

/* Hysteresis: lights above LED_BUS_ON_MV, goes out below LED_BUS_OFF_MV, and
   holds whatever it was doing in between. A bus sitting on the threshold then
   costs no flicker at all, rather than one blink per sample. */
static bool led_bus_connected(void)
{
  uint32_t bus_mv = vbus_millivolts();

  if (bus_mv >= LED_BUS_ON_MV)
  {
    led_bus_lit = true;
  }
  else if (bus_mv < LED_BUS_OFF_MV)
  {
    led_bus_lit = false;
  }

  return led_bus_lit;
}

/* Rewritten unconditionally rather than only on a change: a pin write is a
   single store, and driving it every pass means a line recovers on its own if
   anything else ever disturbs the port. */
void led_service(void)
{
  bool any_running = false;

  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    const pwm_channel_t *channel = pwm_channel((pwm_channel_id_t)i);
    bool running = (channel != NULL) && (pwm_get_state(channel) == PWM_STATE_RUNNING);

    led_write(&led_channel_lines[i], running);
    any_running = any_running || running;
  }

  led_write(&led_status_active, any_running);
  led_write(&led_status_error, led_any_fault());
  led_write(&led_status_bus, led_bus_connected());
}

void led_init(void)
{
  /* Nothing can be running yet, so this settles every line at its idle level
     and takes the first bus reading. */
  led_service();
}
