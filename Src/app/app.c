// Contains setup and loop phases, called in main.c in init and loop.
#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#include "channel.h"
#include "led.h"
#include "pwm.h"
#include "config.h"
#include "main.h"
#include "analog.h"
#include "check.h"

volatile bool error_flag = false; // Global error flag, set to true when something I did not expect occurs.

void app_setup(void) {
  channel_init_all();
  led_init();
  sys.state = SYSTEM_STATE_INIT;
}


static system_state_t prev_state = SYSTEM_STATE_INIT;

void app_loop(void) {
  const bool entered = (sys.state != prev_state);
  prev_state = sys.state;

  switch (sys.state) {
    case SYSTEM_STATE_INIT:
      sys.state = SYSTEM_STATE_CHECK;
      break;

    case SYSTEM_STATE_CHECK: {
      if (entered) {
        check_begin();
      }
      check_result_t result = check_service();
      if (result == CHECK_FAILED) {
        sys.state = SYSTEM_STATE_FAULTED;
      } else if (result == CHECK_PASSED) {
        sys.state = SYSTEM_STATE_STANDBY;
      }
      break;
    }

    case SYSTEM_STATE_STANDBY:
      break;

    case SYSTEM_STATE_ACTIVE:
      break;

    case SYSTEM_STATE_FAULTED:
      led_set_err(true);
      break;

    default:
      error_flag = true;
      sys.state = SYSTEM_STATE_FAULTED;
      break;
  }
}
