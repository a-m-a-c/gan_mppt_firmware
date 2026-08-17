#include "led.h"

#include "config.h"
#include "main.h"

void led_set_channel_a(bool on) {
  HAL_GPIO_WritePin(LED_TOG_1_GPIO_Port, LED_TOG_1_Pin, (GPIO_PinState)!on);
}

void led_set_channel_b(bool on) {
  HAL_GPIO_WritePin(LED_TOG_2_GPIO_Port, LED_TOG_2_Pin, (GPIO_PinState)!on);
}

void led_set_channel_c(bool on) {
  HAL_GPIO_WritePin(LED_TOG_3_GPIO_Port, LED_TOG_3_Pin, (GPIO_PinState)!on);
}

void led_set_channel_d(bool on) {
  HAL_GPIO_WritePin(LED_TOG_4_GPIO_Port, LED_TOG_4_Pin, (GPIO_PinState)!on);
}

void led_set_channel_e(bool on) {
  HAL_GPIO_WritePin(LED_TOG_5_GPIO_Port, LED_TOG_5_Pin, (GPIO_PinState)!on);
}

void led_set_active(bool on) {
  HAL_GPIO_WritePin(LED_ACTIVE_GPIO_Port, LED_ACTIVE_Pin, (GPIO_PinState)on);
}

void led_set_err(bool on) {
  HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, (GPIO_PinState)on);
}

void led_set_out_conn(bool on) {
  HAL_GPIO_WritePin(LED_OUT_CONN_GPIO_Port, LED_OUT_CONN_Pin, (GPIO_PinState)on);
}

static uint32_t lightshow_start_ms;
static bool lightshow_running;

void led_init(void) {
  lightshow_running = false;

  led_set_err(false);
  led_set_out_conn(false);
  led_set_channel_a(false);
  led_set_channel_b(false);
  led_set_channel_c(false);
  led_set_channel_d(false);
  led_set_channel_e(false);
  led_set_active(false);
}

void led_lightshow(bool on) {
  led_init();
  lightshow_running = on;
  lightshow_start_ms = HAL_GetTick();
}

/* Sweeps one lit line along all eight, then flashes them together. */
bool led_lightshow_service(void) {
  uint32_t elapsed;
  uint32_t step;
  bool on;

  if (!lightshow_running) {
    return false;
  }

  elapsed = HAL_GetTick() - lightshow_start_ms;

  if (elapsed >= (LED_SEQUENCE_MS + LED_BLINK_MS)) {
    led_init();
    return false;
  }

  if (elapsed < LED_SEQUENCE_MS) {
    step = (elapsed / LED_SEQUENCE_STEP_MS) % 8U;

    led_set_err(step == 0U);
    led_set_out_conn(step == 1U);
    led_set_channel_a(step == 2U);
    led_set_channel_b(step == 3U);
    led_set_channel_c(step == 4U);
    led_set_channel_d(step == 5U);
    led_set_channel_e(step == 6U);
    led_set_active(step == 7U);
    return true;
  }

  on = ((((elapsed - LED_SEQUENCE_MS) / LED_SEQUENCE_STEP_MS) % 2U) == 0U);

  led_set_err(on);
  led_set_out_conn(on);
  led_set_channel_a(on);
  led_set_channel_b(on);
  led_set_channel_c(on);
  led_set_channel_d(on);
  led_set_channel_e(on);
  led_set_active(on);
  return true;
}
