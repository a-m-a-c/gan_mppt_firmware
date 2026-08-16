/**
  ******************************************************************************
  * @file    app.c
  * @author  Angus Macdonald
  * @brief   Application entry points, called from the generated main().
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "app.h"

#include "channel_telem.h"
#include "command.h"
#include "control.h"
#include "iind.h"
#include "led.h"
#include "main.h"
#include "pwm.h"
#include "serial.h"
#include "analog.h"

void app_setup(void)
{
  // Bring up telemetry. Soft-fails per channel if no sensor answers, so an
  // unpopulated I2C bus leaves the converter running untouched.
  telem_init(&telem_a);
  telem_init(&telem_b);
  telem_init(&telem_c);
  telem_init(&telem_d);
  telem_init(&telem_e);
  telem_start_sweeps();

  control_init();
  analog_init();
  led_init();
  serial_init();

  // Inductor current sensing. Configures the ADCs and the HRTIM sample clock
  // and leaves every channel stopped - nothing converts until iind_start().
  // Returns false until the CubeMX changes in .agents/hardware.md are made;
  // the converter runs without it, so that is not made fatal.
  (void)iind_init();

  // Nothing starts switching here. The host brings channels up over the
  // serial link - init / start / stop, see command.h - or add pwm_init()
  // calls of your own above.
}

void app_loop(void)
{
  telem_service();            /* starts or advances an I2C transfer; never waits */
  analog_service();           /* bus volts + 5 NTCs, one 27 us sweep per 10 ms */

  /* Host -> board -> host, in that order and within one pass, so the "#cfg"
     lines a command triggers already describe the applied result. Keep these
     adjacent and in this order - see command.h. */
  command_service();          /* drain received lines, parse, post requests    */
  control_service();          /* apply every pending setpoint to the driver    */
  command_report_service();   /* queue the "#cfg" set and the telemetry CSV    */

  led_service();              /* pure reads of pwm and analog, no I/O          */
}
