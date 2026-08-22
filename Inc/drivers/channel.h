#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
  SYSTEM_STATE_INIT = 0,
  SYSTEM_STATE_CHECK,
  SYSTEM_STATE_STANDBY,
  SYSTEM_STATE_ACTIVE,
  SYSTEM_STATE_FAULTED
} system_state_t;

typedef enum {
  PWM_STATE_UNINITIALIZED = 0,
  PWM_STATE_STOPPED,
  PWM_STATE_RUNNING,
  PWM_STATE_FAULTED
} pwm_state_t;

typedef struct
{
  float vin_v;
  float iin_a;
  float vout_v;
  float iout_a;
  uint32_t tick_ms; // tick ms when updated
  uint32_t seq;     // Not sure delete
  bool valid;      // Not sure, delete probably
} chan_telem_t;

// Not used yet
typedef struct
{
  float iind_a;
  uint32_t seq;
  bool valid;
} chan_ind_t;

typedef struct
{
  uint32_t frequency_hz;
  uint16_t duty_commanded;
  uint16_t duty_applied;
  uint16_t dead_time_ns;
  pwm_state_t op_state;   /* only UNINITIALIZED / STOPPED / RUNNING */
  volatile bool ocp_latched; /* ISR-written; see carve-out above */
  uint32_t seq;
} chan_pwm_t;

// Represents a channel.
typedef struct
{
  uint32_t id;
  chan_telem_t telem;
  chan_ind_t ind;
  chan_pwm_t pwm;
} channel_t;

// Represents the system.
typedef struct
{
  float vbus_v; /* battery bus, from the ADC divider - shared by all channels */
  uint32_t vbus_seq;
  volatile bool ovp_latched; /* ISR-written; global, hence here not per channel */
  uint32_t sweep_id; /* ++ per completed five-channel telemetry sweep */
  uint32_t tick_ms;
  system_state_t state;
} system_t;

// Our five lovely channels this is the main place to find what you need iykyk
extern channel_t chan_a;
extern channel_t chan_b;
extern channel_t chan_c;
extern channel_t chan_d;
extern channel_t chan_e;
extern system_t sys;
void channel_init_all(void);

channel_t *channel_by_id(uint32_t id);

#endif /* CHANNEL_H */
