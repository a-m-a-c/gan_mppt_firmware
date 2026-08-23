// Contains setup and loop phases, called in main.c in init and loop.
#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#include "channel.h"
#include "system.h"
#include "led.h"
#include "pwm.h"
#include "config.h"
#include "main.h"
#include "analog.h"
#include "check.h"
#include "status.h"
#include "channel_telem.h"
#include "mode.h"

volatile bool error_flag = false; // Global error flag, set to true when something I did not expect occurs.

void app_setup(void) {
  channel_init_all();
  system_init();
  led_init();
  analog_init();
  telem_init(CHANNEL_A);
  telem_init(CHANNEL_B);
  telem_init(CHANNEL_C);
  telem_init(CHANNEL_D);
  telem_init(CHANNEL_E);
  telem_start_sweeps();
  pwm_init(CHANNEL_A);
  pwm_init(CHANNEL_B);
  pwm_init(CHANNEL_C);
  pwm_init(CHANNEL_D);
  pwm_init(CHANNEL_E);
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
      switch (result) {
        case CHECK_RUNNING:
          break;
        case CHECK_FAILED:
          sys.state = SYSTEM_STATE_FAULTED;
          break;
        case CHECK_PASSED:
          sys.state = SYSTEM_STATE_STANDBY;
          break;
      }      
      break;
    }
  
    /* ------------------------------- STANDBY STATE -------------------------------*/
    case SYSTEM_STATE_STANDBY: {
      /* ENTRY BEHAVIOUR */
      if (entered) pwm_stop_all();

      /* ONGOING BEHAVIOUR */
      // Poll for starting requests here perhaps?
      // Update sys.mode here after looking at incoming comms messages?
      uint32_t temp_mode_selection = MODE_SINGLE_CH_CV; // Will come from comm checking

      sys.mode = temp_mode_selection;

      /* TRANSITION LOGIC */
      if (sys.mode != MODE_NONE) sys.state = SYSTEM_STATE_ACTIVE;
      break;
    }

    /* ------------------------------- ACTIVE STATE -------------------------------*/
    case SYSTEM_STATE_ACTIVE: {
      /* ENTRY BEHAVIOUR */
      if (entered) {
        mode_init_result_t init_result = mode_begin(sys.mode);
        switch (init_result) {
          case MODE_INIT_FAULT:
            sys.mode = MODE_NONE;
            sys.state = SYSTEM_STATE_FAULTED;
            break;
          case MODE_INIT_REFUSED:
            sys.mode = MODE_NONE;
            sys.state = SYSTEM_STATE_STANDBY;
            break;
          case MODE_INIT_OK:
            break;
        }
      }
      if (sys.state != SYSTEM_STATE_ACTIVE) break;

      /* ONGOING BEHAVIOUR */
      mode_state_t state = mode_service(sys.mode);

      /* TRANSITION LOGIC */
      switch (state) {
        case MODE_STATE_INIT:
          break;
        case MODE_STATE_RUNNING:
          break;
        case MODE_STATE_FAULTED:
          sys.mode = MODE_NONE;
          sys.state = SYSTEM_STATE_FAULTED;
          break;
        case MODE_STATE_EXIT:
          sys.mode = MODE_NONE;
          sys.state = SYSTEM_STATE_STANDBY;
          break;
      }
      break;
    }

    /* ------------------------------- FAULTED STATE -------------------------------*/  
    case SYSTEM_STATE_FAULTED: {
      /* ENTRY BEHAVIOUR */
      if (entered) pwm_stop_all();

      /* ONGOING BEHAVIOUR */
      // Polling for recovery message, temp for now.
      bool temp_do_recover = true;

      /* TRANSITION LOGIC */
      if (temp_do_recover) sys.state = SYSTEM_STATE_CHECK; // After fault, run checks again.

      break;
    }

    /* ------------------------------- DEFAULT -------------------------------*/
    default: {
      error_flag = true;
      sys.state = SYSTEM_STATE_FAULTED;
      break;
    }
  }

  /* Always on Services*/
  analog_service();
  status_service();
  telem_service();
}
