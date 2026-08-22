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
  pwm_init(CHANNEL_A);
  pwm_init(CHANNEL_B);
  pwm_init(CHANNEL_C);
  pwm_init(CHANNEL_D);
  pwm_init(CHANNEL_E);

  sys.state = SYSTEM_STATE_INIT;
}

static system_state_t prev_state = SYSTEM_STATE_INIT;

void app_loop(void) {
  const bool entered = (sys.state != prev_state);
  prev_state = sys.state;

  switch (sys.state) {
    /* ------------------------------- INIT STATE -------------------------------*/
    case SYSTEM_STATE_INIT: {
      /* TRANSITION LOGIC */
      sys.state = SYSTEM_STATE_CHECK;

      break;
    }
    /* ------------------------------- CHECK STATE -------------------------------*/
    case SYSTEM_STATE_CHECK: {
      /* ENTRY BEHAVIOUR */
      if (entered) check_begin();

      /* ONGOING BEHAVIOUR */
      check_result_t result = check_service();

      /* TRANSITION LOGIC */
      if (result == CHECK_FAILED) sys.state = SYSTEM_STATE_FAULTED;
      if (result == CHECK_PASSED) sys.state = SYSTEM_STATE_STANDBY;
      
      break;
    }
  
    /* ------------------------------- STANDBY STATE -------------------------------*/
    case SYSTEM_STATE_STANDBY: {
      break;
    }
    /* ------------------------------- ACTIVE STATE -------------------------------*/
    case SYSTEM_STATE_ACTIVE: {
      break;
    }
    /* ------------------------------- FAULTED STATE -------------------------------*/  
    case SYSTEM_STATE_FAULTED: {
      /* ENTRY BEHAVIOUR */
      if (entered) {
        pwm_stop(CHANNEL_A);
        pwm_stop(CHANNEL_B);
        pwm_stop(CHANNEL_C);
        pwm_stop(CHANNEL_D);
        pwm_stop(CHANNEL_E);
        led_set_err(true);
      }

      break;
    }
    /* ------------------------------- DEFAULT -------------------------------*/
    default: {
      error_flag = true;
      sys.state = SYSTEM_STATE_FAULTED;
      break;
    }
  }
}
