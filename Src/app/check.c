#include "check.h"
#include "config.h"
#include "app.h"
#include "main.h"
#include <stdbool.h>
#include <stdint.h>

// Internal FSM
typedef enum {
  CHECK_STATE_FLT_LINES,
  CHECK_STATE_WAIT_SWEEP,
  CHECK_STATE_IIND_CALIBRATION,
  CHECK_STATE_DONE,
} check_state_t;

// Check the fault lines are not faulted at startup.
static bool check_flt_lines(void) {
  if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_SET) return false; // CHA
  if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_SET) return false; // CHB
  if (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_4) == GPIO_PIN_SET) return false; // CHC
  if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_SET) return false; // CHD
  if (HAL_GPIO_ReadPin(GPIOG, GPIO_PIN_10) == GPIO_PIN_SET) return false; // CHE
  if (HAL_GPIO_ReadPin(OVP_GPIO_Port, OVP_Pin) == GPIO_PIN_SET) return false; // OVP
  return true;
}

// Calibrate iind against ina228 at startup. Not to be implemented rn.
static bool check_iind_calibration(uint32_t channel_id) {
  return true;
}

static uint32_t check_start_ms;
static check_state_t check_state;

// Begins system checks.
void check_begin(void) {
  check_start_ms = HAL_GetTick();
  check_state = CHECK_STATE_FLT_LINES;
}

// Services the system checks.
check_result_t check_service(void) {
  // Check timeout, defined in config.h
  if (HAL_GetTick() - check_start_ms > CHECK_TIMEOUT_MS) {
    return CHECK_FAILED;
  }

  // Checking FSM
  switch (check_state) {
    case CHECK_STATE_FLT_LINES:
      if (check_flt_lines()) {
        check_state = CHECK_STATE_WAIT_SWEEP;
      } else {
        return CHECK_FAILED;
      }
      break;

    case CHECK_STATE_WAIT_SWEEP:
      // Implement later
      check_state = CHECK_STATE_IIND_CALIBRATION;
      break;

    case CHECK_STATE_IIND_CALIBRATION:
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

    case CHECK_STATE_DONE:
      return CHECK_PASSED;

    default:
      error_flag = true;
      return CHECK_FAILED;
  }

  return CHECK_RUNNING;
}
