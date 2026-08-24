#include "mode_single_ch_cv.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "pwm.h"
#include "pi.h"
#include <stdint.h>

// This mode is intended for bench testing, and the path may be removed for production due to damage risk.
// It will use a voltage input on channel 1, and boost to a constant voltage on the output. 
// There is unknown consequences if there is a battery on the output.
// Use with a resistive load.= 
// Mode will run for a fixed time, then exit.

// Temporary implementation defines, solid ones move to config.h
//#define START_HOLD_TIME_MS 500 // 5 seconds
//#define RUN_TIME_MS 1500
#define VOUT_MV 25000
#define KP 0.006f
#define KI 0.6f
#define SAMPLE_TIME_MS 1
#define DT_MAX_MS (2U * SAMPLE_TIME_MS)
#define MAX_DUTY_CYCLE 750
#define MIN_DUTY_CYCLE 0 
// Scaled with SAMPLE_TIME_MS to hold the same dI/dt: 150/step at 10 ms was
// the bench-proven rate (218 tripped OCP), so 15/step at 1 ms is the same
// 15000 duty/s. See hardware.md.
#define DUTY_SLEW_PER_STEP 15U

static uint32_t start_time_ms;
static uint32_t last_sample_ms;
static bool slew_limited = false;
static pi_t volt_pi;

mode_init_result_t mode_single_ch_cv_begin(void) {
  if (channel_a.telem.valid == false) return MODE_INIT_REFUSED; // Must have a valid telemetry reading.
  if (channel_a.telem.iin_a > 1.0) return MODE_INIT_REFUSED; // Must be less than 1A on the input.
  start_time_ms = HAL_GetTick();

  pi_init(&volt_pi, KP, KI, (float)MIN_DUTY_CYCLE, (float)MAX_DUTY_CYCLE);
  slew_limited = false;
  last_sample_ms = start_time_ms;
  pwm_start(CHANNEL_A);
  return MODE_INIT_OK;
}

mode_state_t mode_single_ch_cv_service(bool stopping) {
  const uint32_t now = HAL_GetTick();

  if (sys.ovp_latched || channel_a.pwm.ocp_latched) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_FAULTED;
  }

  if (stopping) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_EXIT;
  }

  // if (now - start_time_ms >= RUN_TIME_MS) {
  //   pwm_stop(CHANNEL_A);
  //   return MODE_STATE_EXIT;
  // }

  // if (now - start_time_ms < START_HOLD_TIME_MS) {
  //   last_sample_ms = now;
  //   return MODE_STATE_RUNNING;
  // }

  uint32_t dt_ms = now - last_sample_ms;
  if (dt_ms > DT_MAX_MS) {
    dt_ms = DT_MAX_MS;
  }

  if (dt_ms >= SAMPLE_TIME_MS) {
    last_sample_ms = now;

    uint16_t duty = (uint16_t)pi_update(&volt_pi, (float)VOUT_MV, (float)sys.vbus_mv, (float)dt_ms);

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
    pi_track(&volt_pi, (float)channel_a.pwm.duty_applied); // reseed past the slew limiter
  }

  return MODE_STATE_RUNNING;
}
