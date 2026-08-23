#include "mode_single_ch_cv.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "pwm.h"
#include "dev_reporter.h" // Development only - see Inc/dev/dev_reporter.h
#include <stdint.h>

// This mode is intended for bench testing, and the path may be removed for production due to damage risk.
// It will use a voltage input on channel 1, and boost to a constant voltage on the output. 
// There is unknown consequences if there is a battery on the output.
// Use with a resistive load.= 
// Mode will run for a fixed time, then exit.

// Temporary implementation defines, solid ones move to config.h
#define START_HOLD_TIME_MS 1000 // 5 seconds
#define RUN_TIME_MS 20000
#define VOUT_MV 25000
#define KP 0.006f
#define KI 0.6f
#define SAMPLE_TIME_MS 10
#define DT_MAX_MS (2U * SAMPLE_TIME_MS)
#define DEV_RECORD_MS 10
#define MAX_DUTY_CYCLE 750
#define MIN_DUTY_CYCLE 0 
#define DUTY_SLEW_PER_STEP 150U

static uint32_t start_time_ms;
static uint32_t last_sample_ms;
static float integral = 0.0f;
static bool slew_limited = false;

static uint16_t pi_controller(float setpoint, float measurement, uint32_t dt_ms) {
  float error = setpoint - measurement; // error
  float i_term = integral + KI * error * ((float)dt_ms / 1000.0f); // integral term
  float output = KP * error + i_term; // PI output

  if (output > (float)MAX_DUTY_CYCLE) {
    output = (float)MAX_DUTY_CYCLE;
  } else if (output < (float)MIN_DUTY_CYCLE) {
    output = (float)MIN_DUTY_CYCLE;
  } else {
    integral = i_term; // update integral only if within bounds
  }

  return (uint16_t)output;
}

mode_init_result_t mode_single_ch_cv_begin(void) {
  if (channel_a.telem.valid == false) return MODE_INIT_REFUSED; // Must have a valid telemetry reading.
  if (channel_a.telem.iin_a > 1.0) return MODE_INIT_REFUSED; // Must be less than 1A on the input.
  start_time_ms = HAL_GetTick();

  integral = 0.0f;
  slew_limited = false;
  last_sample_ms = start_time_ms;
  dev_reset(); // Development only - a capture must not inherit the last run.
  pwm_start(CHANNEL_A);
  return MODE_INIT_OK;
}

mode_state_t mode_single_ch_cv_service(bool stopping) {
  const uint32_t now = HAL_GetTick();

  // Dev reporting
  static uint32_t last_record_ms;
  if (dev_every_ms(&last_record_ms, DEV_RECORD_MS)) {
    dev_record("vbus_mv", (float)sys.vbus_mv);
    dev_record("duty", (float)channel_a.pwm.duty_applied);
    dev_record("slew", (float)slew_limited); // 1 while the limiter is in charge
  }

  if (sys.ovp_latched || channel_a.pwm.ocp_latched) {
    pwm_stop(CHANNEL_A);
    dev_flush(); // dev reporting blocks for a long time.
    return MODE_STATE_FAULTED;
  }

  if (stopping) {
    pwm_stop(CHANNEL_A);
    dev_flush(); // dev reporting blocks for a long time.
    return MODE_STATE_EXIT;
  }

  if (now - start_time_ms >= RUN_TIME_MS) {
    pwm_stop(CHANNEL_A);
    dev_flush(); // dev reporting blocks for a long time.
    return MODE_STATE_EXIT;
  }

  if (now - start_time_ms < START_HOLD_TIME_MS) {
    last_sample_ms = now;
    return MODE_STATE_RUNNING;
  }

  uint32_t dt_ms = now - last_sample_ms;
  if (dt_ms > DT_MAX_MS) {
    dt_ms = DT_MAX_MS;
  }

  if (dt_ms >= SAMPLE_TIME_MS) {
    last_sample_ms = now;

    uint16_t duty = pi_controller((float)VOUT_MV, (float)sys.vbus_mv, dt_ms);

    const uint16_t applied = channel_a.pwm.duty_applied;
    uint16_t slew_down = 0U;

    if (applied > DUTY_SLEW_PER_STEP) {
      slew_down = applied - DUTY_SLEW_PER_STEP;
    }

    slew_limited = false;

    if (duty > (applied + DUTY_SLEW_PER_STEP)) {
      duty = applied + DUTY_SLEW_PER_STEP;
      slew_limited = true;
    } else if (duty < slew_down) {
      duty = slew_down;
      slew_limited = true;
    }

    (void)pwm_set_duty_cycle(CHANNEL_A, duty);
  }

  return MODE_STATE_RUNNING;
}
