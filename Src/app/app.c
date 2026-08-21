// Contains setup and loop phases, called in main.c in init and loop.
#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#include "analog.h"
#include "channel.h"
#include "channel_telem.h"
#include "command.h"
#include "config.h"
#include "control.h"
#include "iind.h"
#include "led.h"
#include "main.h"
#include "pwm.h"
#include "serial.h"

void app_setup(void) {
  channel_init_all();

  // Telemetry. Soft-fails per channel if no sensor answers, so an unpopulated
  // I2C bus leaves the converter running untouched.
  telem_init(&telem_a);
  telem_init(&telem_b);
  telem_init(&telem_c);
  telem_init(&telem_d);
  telem_init(&telem_e);
  telem_start_sweeps();

  control_init();
  analog_init();
  serial_init();

  // Inductor current sensing. Returns false until the CubeMX changes in
  // .agents/hardware.md are made; the converter runs without it.
  (void)iind_init();

  led_init();
  led_lightshow(true);

  pwm_init(CHANNEL_A);
  pwm_start(CHANNEL_A);
}

void app_loop(void) {
  telem_service();  /* starts or advances an I2C transfer; never waits */
  analog_service(); /* bus volts + 5 NTCs, one sweep per ANALOG_PERIOD_MS */

  /* Host -> board -> host, in that order and within one pass, so the "#cfg"
     lines a command triggers already describe the applied result. Keep these
     adjacent and in this order - see command.h. */
  command_service();        /* drain received lines, parse, post requests */
  control_service();        /* apply every pending setpoint to the driver */
  command_report_service(); /* queue the "#cfg" set and the telemetry CSV */

  led_lightshow_service();
}
