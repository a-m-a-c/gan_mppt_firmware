/**
  ******************************************************************************
  * @file    channel_telem.h
  * @author  Angus Macdonald
  * @brief   Per-channel converter telemetry (paired INA228 sensors).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef CHANNEL_TELEM_H
#define CHANNEL_TELEM_H

#include <stdbool.h>
#include <stdint.h>
#include "ina228.h"
#include "pwm.h"

#define TELEM_HISTORY_DEPTH 32U

typedef struct
{
  uint32_t tick_ms; /* HAL_GetTick() when the sample was taken */
  float vin_v;
  float iin_a;
  float vout_v;
  float iout_a;
} telem_sample_t;

/* Telemetry state for one converter channel: an INA228 on the input side and
 * one on the output side. telem_update() refreshes `latest` and pushes it
 * into `history` (ring buffer, newest first via telem_history_get). */
typedef struct
{
  pwm_channel_id_t number;
  ina228_t input;
  ina228_t output;
  telem_sample_t latest;
  bool valid;           /* last init/update fully succeeded */
  uint32_t error_count; /* total failed inits/updates */
  telem_sample_t history[TELEM_HISTORY_DEPTH];
  uint32_t history_head;
  uint32_t history_count;
} telem_channel_t;

/* The 5 converter channels, defined in channel_telem.c. */
extern telem_channel_t telem_a;
extern telem_channel_t telem_b;
extern telem_channel_t telem_c;
extern telem_channel_t telem_d;
extern telem_channel_t telem_e;

/* All functions soft-fail (return false, bump error_count) on sensor/I2C
 * errors - telemetry loss must never halt the converter. */
bool telem_init(telem_channel_t *t);

/* Blocking refresh of one channel: four I2C reads, ~2 ms. Kept for bench use
 * and one-shot reads. The steady-state path is telem_service(). */
bool telem_update(telem_channel_t *t);

/* Enables the I2C1 interrupt and arms the first sweep. Call once from
 * app_setup(), after every telem_init() - those use blocking reads and must
 * not overlap the interrupt-driven path. */
void telem_start_sweeps(void);

/* Non-blocking sweep of all five channels, one I2C transfer at a time, driven
 * by the I2C1 interrupt. Call every pass of the main loop; it starts a sweep
 * every TELEM_SWEEP_PERIOD_MS and advances the one in flight, returning in a
 * few microseconds either way.
 *
 * A channel's four values are always committed together, so `latest` and
 * `valid` never show a half-updated channel. Different channels are sampled up
 * to one sweep apart - telem_sweep_age_ms() says how stale the set is. */
void telem_service(void);

/* Increments once per completed sweep. A consumer that must act on fresh data
 * only - the MPPT loop - watches this rather than a timer. */
uint32_t telem_sweep_id(void);

/* Milliseconds since the last sweep completed. */
uint32_t telem_sweep_age_ms(void);

/* I2C1 completion hooks, called from the HAL callbacks in interrupts.c. Not
 * for application use. */
void telem_i2c_complete(void);
void telem_i2c_error(void);

/* age 0 = newest stored sample, 1 = one before it, ... Returns false once
 * age reaches the number of samples stored so far. */
bool telem_history_get(const telem_channel_t *t, uint32_t age, telem_sample_t *out);

#endif /* CHANNEL_TELEM_H */
