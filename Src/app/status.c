#include "status.h"
#include "led.h"
#include "channel.h"
#include "system.h"
#include "config.h"

void status_service(void) {
  led_set_err(sys.state == SYSTEM_STATE_FAULTED);
  led_set_active(sys.state == SYSTEM_STATE_ACTIVE);

  /* Between the thresholds neither branch runs, so the LED holds its state. */
  if (sys.vbus_mv > LED_BUS_ON_MV) {
    led_set_out_conn(true);
  } else if (sys.vbus_mv < LED_BUS_OFF_MV) {
    led_set_out_conn(false);
  }

  led_set_channel_a(channel_a.pwm.op_state == PWM_STATE_RUNNING);
  led_set_channel_b(channel_b.pwm.op_state == PWM_STATE_RUNNING);
  led_set_channel_c(channel_c.pwm.op_state == PWM_STATE_RUNNING);
  led_set_channel_d(channel_d.pwm.op_state == PWM_STATE_RUNNING);
  led_set_channel_e(channel_e.pwm.op_state == PWM_STATE_RUNNING);


}