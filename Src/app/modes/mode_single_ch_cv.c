#include "mode_single_ch_cv.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "pwm.h"
#include "pi.h"
#include <stdint.h>

// Do not run with a battery, I reckon it would skitz out.

// Internal state, may be used as template for other modes, but kept private to prevent misuse.
// mode_state_t is reported as running always.
// Faults are immediate exit.
typedef enum {
  STATE_STARTUP,
  STATE_RUNNING,
  STATE_STOPPING,
  STATE_EXIT
} internal_state_t;

// Mode parameters
#define VOUT_MV 25000
#define KP 0.006f
#define KI 0.6f
#define SAMPLE_TIME_MS 1
#define DT_MAX_MS (2U * SAMPLE_TIME_MS)
#define MAX_DUTY_CYCLE 750
#define MIN_DUTY_CYCLE 0 
#define DUTY_SLEW_PER_STEP 15U

// Static states
static uint32_t start_time_ms;
static uint32_t last_sample_ms;
static pi_t volt_pi;
static internal_state_t internal_state;

// Called at start of mode, before first service pass.
mode_request_result_t mode_single_ch_cv_begin(void) {
  // Checks
  if (channel_a.telem.valid == false) return MODE_INIT_REFUSED;
  if (channel_a.telem.iin_a > 1.0) return MODE_INIT_REFUSED;

  // Setup clean slate
  internal_state = STATE_STARTUP;
  start_time_ms = HAL_GetTick();
  last_sample_ms = start_time_ms;
  pi_init(&volt_pi, KP, KI, (float)MIN_DUTY_CYCLE, (float)MAX_DUTY_CYCLE);
  pwm_start(CHANNEL_A);
  return MODE_INIT_OK;
}

// Called repeatedly while mode is active.
mode_state_t mode_single_ch_cv_service(bool stopping) {
  const uint32_t now = HAL_GetTick();

  /* --------------------- FAULT CHECKING --------------------- */
  if (sys.ovp_latched || channel_a.pwm.ocp_latched) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_FAULTED;
  }

  /* --------------------- STATE MACHINE --------------------- */
  switch (internal_state) {
    /* --------------------- STARTUP STATE --------------------- */
    case STATE_STARTUP:
      if (stopping) {
        internal_state = STATE_STOPPING;
        break;
      }
      internal_state = STATE_RUNNING;
      break;

    /* --------------------- RUNNING STATE --------------------- */
    case STATE_RUNNING:
      if (stopping) {
        internal_state = STATE_STOPPING;
        break;
      }

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

        if (duty > (applied + DUTY_SLEW_PER_STEP)) {
          duty = applied + DUTY_SLEW_PER_STEP;
        } else if (duty < slew_down) {
          duty = slew_down;
        }

        (void)pwm_set_duty_cycle(CHANNEL_A, duty);
        pi_track(&volt_pi, (float)channel_a.pwm.duty_applied); // reseed past the slew limiter
      }
      break;

    /* --------------------- STOPPING STATE --------------------- */
    case STATE_STOPPING:
      pwm_stop(CHANNEL_A);
      internal_state = STATE_EXIT;
      break;

    /* --------------------- EXIT STATE --------------------- */
    case STATE_EXIT:
      return MODE_STATE_EXIT;
  }  

  return MODE_STATE_RUNNING;
}
