#include "check.h"
#include "config.h"
#include "app.h"
#include "main.h"
#include "pwm.h"
#include <stdbool.h>
#include <stdint.h>

// Internal FSM
typedef enum {
  CHECK_STATE_WAIT_SWEEP,
  CHECK_STATE_IIND_CALIBRATION,
  CHECK_STATE_DONE,
} check_state_t;

// Calibrate iind against ina228 at startup. Not to be implemented rn.
static bool check_iind_calibration(uint32_t channel_id) {
  return true;
}

static uint32_t check_start_ms;
static check_state_t check_state;

// Begins system checks.
void check_begin(void) {
  check_start_ms = HAL_GetTick();
  check_state = CHECK_STATE_WAIT_SWEEP;
}

// Services the system checks.
check_result_t check_service(void) {
  // A fault at any CHECK stage fails it, even if the input pulse has ended.
  if (pwm_faults_present()) {
    return CHECK_FAILED;
  }

  // Check timeout, defined in config.h
  if (HAL_GetTick() - check_start_ms > CHECK_TIMEOUT_MS) {
    return CHECK_FAILED;
  }

  // Checking FSM
  switch (check_state) {
    case CHECK_STATE_WAIT_SWEEP:
      // Implement later
      check_state = CHECK_STATE_IIND_CALIBRATION;
      break;

    case CHECK_STATE_IIND_CALIBRATION: {
      // Implement later
      int res1 = check_iind_calibration(CHANNEL_A);
      int res2 = check_iind_calibration(CHANNEL_B);
      int res3 = check_iind_calibration(CHANNEL_C);
      int res4 = check_iind_calibration(CHANNEL_D);
      int res5 = check_iind_calibration(CHANNEL_E);
      if (res1 && res2 && res3 && res4 && res5) {
        check_state = CHECK_STATE_DONE;
      } else {
        return CHECK_FAILED;
      }
      break;
    }

    case CHECK_STATE_DONE:
      return CHECK_PASSED;

    default:
      error_flag = true;
      return CHECK_FAILED;
  }

  return CHECK_RUNNING;
}
