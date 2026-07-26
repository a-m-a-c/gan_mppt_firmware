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
  float temp_in_c;  /* sensor die temperatures - slow-moving, not in history */
  float temp_out_c;
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
bool telem_update(telem_channel_t *t);

/* age 0 = newest stored sample, 1 = one before it, ... Returns false once
 * age reaches the number of samples stored so far. */
bool telem_history_get(const telem_channel_t *t, uint32_t age, telem_sample_t *out);

#endif /* CHANNEL_TELEM_H */
