#include "mode_single_ch_mppt.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "pwm.h"
#include "pi.h"
#include <stdint.h>

/* P&O on channel A's input voltage. A 10 Hz hill climb moves vin_target_mv;
   a PI loop paced by telemetry holds vin on it by moving duty.

   Perturbing the setpoint rather than the duty keeps the two jobs apart: the
   climb decides where to sit, the PI decides how to get there and holds it
   while the source drifts underneath. */

// Mode parameters
#define PO_PERIOD_MS 100U
#define PO_STEP_MV   100U
#define KP 0.02f
#define KI 0.4f
#define MAX_DUTY_CYCLE 700
#define MIN_DUTY_CYCLE 0
#define DUTY_SLEW_PER_STEP 20U

/* PO_PERIOD_MS is now a floor, not a cadence: a step also waits for a sample
   taken after the last one and for the PI to have arrived. Stepping on the
   timer alone let the setpoint outrun the loop - at KI 0.4 and -9.85 mV/duty
   (measured, duty 475-575 of the 2026-08-29 sweep) the PI closes at
   0.4 * 9.85 = 3.94 mV/s per mV of error, so a 100 mV per 100 ms walk settles
   1000 / 3.94 = 254 mV behind and every power reading is dominated by the lag
   rather than by the perturbation. */

// Arrived: the PI is inside this band of the setpoint. 30 mV is ~3 duty units.
#define PO_ARRIVED_MV 30U

/* If it can never arrive - duty on a rail, or the source moving faster than
   the loop - step anyway rather than freezing on the setpoint forever. */
#define PO_STALL_MS (10U * PO_PERIOD_MS)

/* KP/KI are duty per mV, so they compare directly with mode_single_ch_cv.c.
   The 2026-08-29 sweep gives about -4.7 mV of vin per duty unit (5.5 -> 2.2 V
   across 700 units), so a deadbeat correction is 0.21 duty/mV. KP is 10% of
   that; KI closes a 100 mV error in 0.4 * 0.1 * 25 -> about 0.5 s.

   With the gate below, a step now costs ln(100/30) / (KI * 9.85) = 305 ms, so
   the climb self-paces at ~3 Hz. KI ~1.2 would bring it back to the 10 Hz
   floor. Raising it is safe now that the gate stops the setpoint running away,
   but it has not been tried on the bench - tune it there, not here. */

/* 20 per 40 ms sample is 500 duty/s. The bench measured 15000 duty/s clean,
   and hardware.md derates a bench constant by ~6x for a real array; this sits
   5x below even that. It still allows 0.5% duty per sample, far more than
   tracking needs. */

// 3 * TELEM_SWEEP_PERIOD_MS. Two missed sweeps means the I2C chain has stalled.
#define TELEM_MAX_AGE_MS 120U
#define DT_MAX_MS        TELEM_MAX_AGE_MS

// Current the channel must be below before the mode may start.
#define IIN_IDLE_MAX_A 1.0f

/* Bench ceiling. The 2.5 ohm fixture cannot be driven past ~10.5 V by the
   panel, so 30 V means the load is gone. RAISE THIS BEFORE RUNNING INTO A
   BATTERY - the real bus is 32.5-54.6 V and this would fault on the first
   pass. The 55 V hardware OVP is the backstop; this only catches it sooner. */
#define MAX_OUTPUT_MV 30000U

/* Fractional-Voc seed: start the loop at a deliberate error pointing the way
   P&O is about to walk. Seeding at the measured vin gives exactly zero error,
   so the PI commands nothing and the climb has to bootstrap against the
   flattest part of the curve - 0.96 mV/duty at duty 0 against 9.85 at the knee
   (2026-08-29 sweep), where one 100 mV step takes 2.6 * ln(100/30) = 3.1 s to
   arrive and the mode looks dead for the first several seconds.

   That sweep puts the MPP at 4.648 V against 5.510 V at duty 0, a ratio of
   0.844. High for silicon (0.76-0.80) because vin at duty 0 is not true open
   circuit - the body diode already feeds ~1.3 A into the fixture. 0.8 is used
   for margin: seeding below the MPP starts on the side P&O walks toward.

   The first correction is large, so the ramp to it is the slew limiter's job,
   not the gains': 1102 mV of error commands 39 duty units, capped to 20 per
   sample = 500 duty/s, so turn-on is an open-loop ramp of about a second. */
#define PO_SEED_FRACTION 0.8f

// Target clamps. The upper bound is the large array's Voc (hardware.md).
#define VIN_TARGET_MIN_MV 2000
#define VIN_TARGET_MAX_MV 30500

static uint32_t last_po_ms;
static uint32_t last_telem_tick;
static uint16_t vin_target_mv;
static float last_pin_w;
static int8_t po_dir;      // +1 raises the target, -1 lowers it
static bool first_step;    // the first climb has no previous power to compare
static pi_t vin_pi;

static bool telem_is_fresh(uint32_t now) {
  return channel_a.telem.valid && ((now - channel_a.telem.tick_ms) <= TELEM_MAX_AGE_MS);
}

static uint32_t abs_diff(uint32_t a, uint32_t b) {
  return (a > b) ? (a - b) : (b - a);
}

// Every exit stops the channel and withdraws the published setpoint.
static mode_state_t finish(mode_state_t state) {
  pwm_stop(CHANNEL_A);
  vin_target_mv = 0U;
  return state;
}

uint16_t mode_single_ch_mppt_target_mv(void) {
  return vin_target_mv;
}

static uint16_t clamp_target_mv(float mv) {
  if (mv < (float)VIN_TARGET_MIN_MV) return (uint16_t)VIN_TARGET_MIN_MV;
  if (mv > (float)VIN_TARGET_MAX_MV) return (uint16_t)VIN_TARGET_MAX_MV;
  return (uint16_t)mv;
}

// Called at start of mode, before first service pass.
mode_request_result_t mode_single_ch_mppt_begin(void) {
  const uint32_t now = HAL_GetTick();

  // Checks
  if (!telem_is_fresh(now)) return MODE_INIT_REFUSED;

  last_po_ms = now;
  last_telem_tick = channel_a.telem.tick_ms;
  last_pin_w = 0.0f;
  first_step = true;

  // Fraction of the present open-circuit voltage, not the voltage itself.
  vin_target_mv = clamp_target_mv(channel_a.telem.vin_v * 1000.0f * PO_SEED_FRACTION);

  // Down first: an idle panel sits near Voc and the MPP is below it.
  po_dir = -1;

  pi_init(&vin_pi, KP, KI, (float)MIN_DUTY_CYCLE, (float)MAX_DUTY_CYCLE);

  /* pwm_start applies PWM_DEFAULT_DUTY_CYCLE (0) before enabling the outputs,
     so the mode always begins from pass-through, and refuses on a latched
     fault rather than tracking against a channel that never switches. */
  if (!pwm_start(CHANNEL_A)) return MODE_INIT_REFUSED;

  return MODE_INIT_OK;
}

// Called repeatedly while mode is active. Runs until stopped.
mode_state_t mode_single_ch_mppt_service(bool stopping) {
  const uint32_t now = HAL_GetTick();

  if (sys.ovp_latched || channel_a.pwm.ocp_latched) {
    return finish(MODE_STATE_FAULTED);
  }

  if (stopping) {
    return finish(MODE_STATE_EXIT);
  }

  if (!telem_is_fresh(now)) {
    return finish(MODE_STATE_FAULTED);
  }

  // sys.vbus_mv, not telem.vout_v: 1 ms (ANALOG_PERIOD_MS) against the
  // INA228's 40 ms. A 40 ms old sample cannot guard a lost load.
  if (sys.vbus_mv >= MAX_OUTPUT_MV) {
    return finish(MODE_STATE_FAULTED);
  }

  /* P&O: moves the setpoint, never the duty. It judges only a sample taken
     after its own last step, the way mode_single_ch_iv_sweep.c dwells before
     reading, and only once the PI has closed on that step. */
  const uint32_t vin_mv = (uint32_t)(channel_a.telem.vin_v * 1000.0f);
  const bool dwelled = (now - last_po_ms) >= PO_PERIOD_MS;
  const bool sampled = (int32_t)(channel_a.telem.tick_ms - last_po_ms) > 0;
  const bool arrived = abs_diff(vin_mv, vin_target_mv) <= PO_ARRIVED_MV;
  const bool stalled = (now - last_po_ms) >= PO_STALL_MS;

  if (dwelled && sampled && (arrived || stalled)) {
    last_po_ms = now;
    const float pin_w = channel_a.telem.vin_v * channel_a.telem.iin_a;

    // Keep walking while power improves, turn around when it does not.
    if (!first_step && (pin_w < last_pin_w)) {
      po_dir = (int8_t)-po_dir;
    }
    first_step = false;
    last_pin_w = pin_w;

    const int32_t stepped = (int32_t)vin_target_mv + (po_dir * (int32_t)PO_STEP_MV);
    const uint16_t clamped = clamp_target_mv((float)stepped);

    /* Turn around at a clamp too. Pushing further into one leaves power
       unchanged, which is not a fall, so the test above would never flip and
       the climb would sit on the rail. */
    if (clamped != (uint16_t)stepped) {
      po_dir = (int8_t)-po_dir;
    }
    vin_target_mv = clamped;
  }

  /* PI: one update per telemetry sample. vin comes from nowhere else, so this
     is the loop's real rate - a faster one would integrate the same sample
     repeatedly and multiply KI by however many times it did so. */
  if (channel_a.telem.tick_ms != last_telem_tick) {
    uint32_t dt_ms = channel_a.telem.tick_ms - last_telem_tick;
    last_telem_tick = channel_a.telem.tick_ms;
    if (dt_ms > DT_MAX_MS) {
      dt_ms = DT_MAX_MS;
    }

    /* Measurement and setpoint are passed the other way round on purpose. A
       boost draws harder as duty rises, so vin FALLS as duty rises - the plant
       gain is negative and pi_update() is written for a positive one. Swapping
       the pair negates the error, which is the inversion, and leaves KP/KI
       positive and comparable with mode_single_ch_cv.c. */
    uint16_t duty = (uint16_t)pi_update(&vin_pi, channel_a.telem.vin_v * 1000.0f,
                                        (float)vin_target_mv, (float)dt_ms);

    const uint16_t applied = channel_a.pwm.duty_applied;
    uint16_t slew_down = 0U;

    if (applied > DUTY_SLEW_PER_STEP) {
      slew_down = applied - DUTY_SLEW_PER_STEP;
    }

    if (duty > (applied + DUTY_SLEW_PER_STEP)) {
      duty = (uint16_t)(applied + DUTY_SLEW_PER_STEP);
    } else if (duty < slew_down) {
      duty = slew_down;
    }

    (void)pwm_set_duty_cycle(CHANNEL_A, duty);
    pi_track(&vin_pi, (float)channel_a.pwm.duty_applied); // reseed past the slew limiter
  }

  return MODE_STATE_RUNNING;
}
