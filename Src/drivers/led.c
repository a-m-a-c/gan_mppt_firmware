/**
  ******************************************************************************
  * @file    led.c
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
#include "led.h"

#include "main.h"

#define LED_HALF_PERIOD_MS (LED_TOGGLE_PERIOD_MS / 2U)

/* Tick at which the lines last changed state. */
static uint32_t led_last_toggle_ms;

void led_init(void)
{
  HAL_GPIO_WritePin(LED_TOG_1_GPIO_Port, LED_TOG_1_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_TOG_2_GPIO_Port, LED_TOG_2_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_TOG_3_GPIO_Port, LED_TOG_3_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_TOG_4_GPIO_Port, LED_TOG_4_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_TOG_5_GPIO_Port, LED_TOG_5_Pin, GPIO_PIN_RESET);

  /* Status LEDs. Flashed alongside the toggle lines for bring-up only - once
     these carry real state, drop them from this module. */
  HAL_GPIO_WritePin(LED_ACTIVE_GPIO_Port, LED_ACTIVE_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_OUT_CONN_GPIO_Port, LED_OUT_CONN_Pin, GPIO_PIN_RESET);

  led_last_toggle_ms = HAL_GetTick();
}

void led_toggle_service(void)
{
  uint32_t now = HAL_GetTick();

  /* Unsigned subtraction, so this stays correct across the 32-bit tick
     wrap at ~49.7 days. */
  if ((now - led_last_toggle_ms) < LED_HALF_PERIOD_MS)
  {
    return;
  }

  /* Resync to now rather than advancing by a fixed step: if a caller ever
     starves the service, that costs a late edge instead of a burst of
     catch-up toggles. */
  led_last_toggle_ms = now;

  HAL_GPIO_TogglePin(LED_TOG_1_GPIO_Port, LED_TOG_1_Pin);
  HAL_GPIO_TogglePin(LED_TOG_2_GPIO_Port, LED_TOG_2_Pin);
  HAL_GPIO_TogglePin(LED_TOG_3_GPIO_Port, LED_TOG_3_Pin);
  HAL_GPIO_TogglePin(LED_TOG_4_GPIO_Port, LED_TOG_4_Pin);
  HAL_GPIO_TogglePin(LED_TOG_5_GPIO_Port, LED_TOG_5_Pin);

  HAL_GPIO_TogglePin(LED_ACTIVE_GPIO_Port, LED_ACTIVE_Pin);
  HAL_GPIO_TogglePin(LED_ERR_GPIO_Port, LED_ERR_Pin);
  HAL_GPIO_TogglePin(LED_OUT_CONN_GPIO_Port, LED_OUT_CONN_Pin);
}

void led_delay_ms(uint32_t ms)
{
  uint32_t start = HAL_GetTick();

  while ((HAL_GetTick() - start) < ms)
  {
    led_toggle_service();
  }
}
