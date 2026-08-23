#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
  PWM_STATE_UNINITIALIZED = 0,
  PWM_STATE_STOPPED,
  PWM_STATE_RUNNING,
  PWM_STATE_FAULTED
} pwm_state_t;

/* Filled in by channel_telem.c from the channel's INA228 pair. This is the
   only home for the numbers - the driver keeps no second copy. */
typedef struct
{
  float vin_v;
  float iin_a;
  float vout_v;
  float iout_a;
  uint32_t tick_ms; /* HAL_GetTick() when this sample was committed */
  bool valid;       /* the last update completed all four reads */
} chan_telem_t;

typedef struct
{
  uint32_t frequency_hz;
  uint16_t duty_commanded;
  uint16_t duty_applied;
  uint16_t dead_time_ns;
  pwm_state_t op_state;
  volatile bool ocp_latched; /* ISR-written; see carve-out above */
} chan_pwm_t;

// Represents a channel.
typedef struct
{
  uint32_t id;
  chan_telem_t telem;
  chan_pwm_t pwm;
} channel_t;

// Our five lovely channels this is the main place to find what you need iykyk
extern channel_t channel_a;
extern channel_t channel_b;
extern channel_t channel_c;
extern channel_t channel_d;
extern channel_t channel_e;

void channel_init_all(void);

channel_t *channel_by_id(uint32_t id);

#endif /* CHANNEL_H */
