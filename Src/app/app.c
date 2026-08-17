// Contains setup and loop phases, called in main.c in init and loop.
#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#include "channel.h"
#include "led.h"
#include "pwm.h"
#include "config.h"
#include "main.h"

void app_setup(void) {
  channel_init_all();
  led_init();
  led_lightshow(true);
  pwm_init(CHANNEL_A);
  pwm_start(CHANNEL_A);
}

static uint16_t target_duty_cycle = 300; //20 %
static uint16_t current_duty_cycle = 0;

void app_loop(void) {
  if (led_lightshow_service()) {
    return;
  }
  while (current_duty_cycle <= target_duty_cycle) {
    pwm_set_duty_cycle(CHANNEL_A, current_duty_cycle);
    current_duty_cycle += 10;
    HAL_Delay(200);
  }

}
