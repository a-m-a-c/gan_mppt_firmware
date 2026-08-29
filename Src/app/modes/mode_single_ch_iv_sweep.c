#include "mode_single_ch_iv_sweep.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "pwm.h"
#include <stdint.h>

/* Walks channel A's duty between the 0% pass-through state and MAX_DUTY_CYCLE,
   dwelling on each step until telemetry has produced a sample taken after it.
   One cycle is 0 -> MAX -> 0; SWEEP_CYCLES of them run back to back.

   Sweeping both ways is what makes the two traversals comparable: a curve that
   does not retrace is hysteresis, and it separates the source's own drift
   (panel heating, a bench supply's thermal loop) from anything direction
   dependent in the converter. A single up-sweep cannot tell the two apart.

   stream.c publishes vin/iin, so the curve comes out over the wire. */

// Bench target: one string into a 2.5 ohm 60 W resistor.
// Voc 5.952 V, Vmpp 5.3844 V, Isc 8.66 A, Impp 8.22 A, Pmpp 44.3 W.

/* A boost presents its source R_in = R_load * (1-D)^2, so duty is what walks
   the panel along its curve. With the 2.5 ohm load: MPP (5.3844/8.22 =
   0.655 ohm) sits at D = 1 - sqrt(0.655/2.5) = 48.8%, and the Isc end
   (~0.1 ohm) at 80.0%. Under PWM_MAX_DUTY_CYCLE (850), which is rejected
   outright. Duty is tenths of a percent - PWM_DUTY_SCALE is 1000.

   The Voc end is unreachable with the load fitted: at D = 0 the panel already
   sees 2.5 ohm and delivers ~2.4 A. */
#define MAX_DUTY_CYCLE 700U

// 0.5%. 800/5 = 160 steps. 5 units per 100 ms is 50 duty/s against the
// 15000 duty/s measured clean on the bench, so OCP slew is not in play.
#define DUTY_CYCLE_STEP 5U

// >= 2 * TELEM_SWEEP_PERIOD_MS (40 ms), so a post-step sample always exists
// before the next step. 160 steps * 100 ms = 16 s sweep.
#define DUTY_STEP_INTERVAL_MS 100U

// 3 * TELEM_SWEEP_PERIOD_MS. Two missed sweeps means the I2C chain has stalled
// and the guards below would be reading a frozen sample.
#define TELEM_MAX_AGE_MS 120U

/* The panel makes at most 44.3 W, so into 2.5 ohm the output cannot exceed
   sqrt(44.3 * 2.5) = 10.5 V. Past 15 V the load is gone and an unloaded boost
   runs toward Vin/(1-D). The 55 V hardware OVP is the backstop; this only
   catches it sooner. */
#define MAX_OUTPUT_MV 15000U

// Current the channel must be below before a sweep may start.
#define IIN_IDLE_MAX_A 1.0f

/* One cycle is 2 * (MAX_DUTY_CYCLE / DUTY_CYCLE_STEP) steps, so at 700, 5 and
   100 ms that is 280 steps = 28 s, and ten of them run for 280 s. The host has
   to be told to capture at least that long - the sweep does not stop early. */
#define SWEEP_CYCLES 10U

static uint32_t last_duty_step_time_ms;
static uint16_t cycles_done;
static bool descending;

static bool telem_is_fresh(uint32_t now) {
  return channel_a.telem.valid && ((now - channel_a.telem.tick_ms) <= TELEM_MAX_AGE_MS);
}

mode_request_result_t mode_single_ch_iv_sweep_begin(void) {
  const uint32_t now = HAL_GetTick();

  // Checks
  if (!telem_is_fresh(now)) return MODE_INIT_REFUSED;

  last_duty_step_time_ms = now;
  cycles_done = 0;
  descending = false;

  /* pwm_start applies PWM_DEFAULT_DUTY_CYCLE (0) before enabling the outputs,
     so the sweep always begins from pass-through whatever the last mode left
     behind. It refuses on a latched fault - without this the sweep would run
     to completion against a channel that never switched. */
  if (!pwm_start(CHANNEL_A)) return MODE_INIT_REFUSED;

  return MODE_INIT_OK;
}

/* MODE_STATE_EXIT means the sweep finished; MODE_STATE_FAULTED means it was
   abandoned part way and the curve is truncated. The host cannot otherwise
   tell the two apart. */
mode_state_t mode_single_ch_iv_sweep_service(bool stopping) {
  const uint32_t now = HAL_GetTick();

  if (sys.ovp_latched || channel_a.pwm.ocp_latched) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_FAULTED;
  }

  if (stopping) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_EXIT;
  }

  if (!telem_is_fresh(now)) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_FAULTED;
  }

  // sys.vbus_mv, not telem.vout_v: 1 ms cadence (ANALOG_PERIOD_MS) against the
  // INA228's 40 ms. A 40 ms old sample cannot guard an unloaded ramp.
  if (sys.vbus_mv >= MAX_OUTPUT_MV) {
    pwm_stop(CHANNEL_A);
    return MODE_STATE_FAULTED;
  }

  /* Signed difference so the comparison survives a tick wrap: true once
     telemetry has committed a sample later than the last step. */
  const bool dwelled = (now - last_duty_step_time_ms) >= DUTY_STEP_INTERVAL_MS;
  const bool sampled = (int32_t)(channel_a.telem.tick_ms - last_duty_step_time_ms) > 0;

  if (dwelled && sampled) {
    const uint16_t duty = channel_a.pwm.duty_applied;

    /* Turn around here rather than the moment the end is reached, so the two
       extremes are dwelled on and sampled like every other step. Turning early
       would make them the only points on the curve without a settled sample. */
    if (!descending && (duty >= MAX_DUTY_CYCLE)) {
      descending = true;
    } else if (descending && (duty == 0U)) {
      descending = false;
      cycles_done++;
      if (cycles_done >= SWEEP_CYCLES) {
        pwm_stop(CHANNEL_A);
        return MODE_STATE_EXIT;
      }
    }

    uint16_t new_duty;
    if (descending) {
      // Clamped to 0 rather than wrapped, and it lands exactly on 0 whether or
      // not MAX_DUTY_CYCLE divides by the step - which the turn above tests for.
      new_duty = (duty > DUTY_CYCLE_STEP) ? (uint16_t)(duty - DUTY_CYCLE_STEP) : 0U;
    } else {
      new_duty = (uint16_t)(duty + DUTY_CYCLE_STEP);
      if (new_duty > MAX_DUTY_CYCLE) new_duty = MAX_DUTY_CYCLE;
    }

    // A rejection leaves duty_applied untouched, so the sweep would sit on this
    // step forever rather than fail.
    if (!pwm_set_duty_cycle(CHANNEL_A, new_duty)) {
      pwm_stop(CHANNEL_A);
      return MODE_STATE_FAULTED;
    }
    last_duty_step_time_ms = now;
  }

  return MODE_STATE_RUNNING;
}
