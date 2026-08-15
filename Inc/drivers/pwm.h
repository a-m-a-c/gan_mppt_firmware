/**
  ******************************************************************************
  * @file    pwm.h
  * @author  Angus Macdonald
  * @brief   PWM channel control (HRTIM).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef PWM_H
#define PWM_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum
{
  PWM_CHANNEL_A = 0,
  PWM_CHANNEL_B,
  PWM_CHANNEL_C,
  PWM_CHANNEL_D,
  PWM_CHANNEL_E,
  PWM_CHANNEL_COUNT
} pwm_channel_id_t;

/* Effective channel state as reported by pwm_get_state(). FAULTED is derived
 * from the latched fault flags, never stored - see op_state below. */
typedef enum
{
  PWM_STATE_UNINITIALIZED = 0,
  PWM_STATE_STOPPED,
  PWM_STATE_RUNNING,
  PWM_STATE_FAULTED
} pwm_state_t;

/* Duty cycles are carried in tenths of a percent, so 1000 is 100%. A unit,
 * not a setting - the tunable defaults and limits live in config.h. */
#define PWM_DUTY_SCALE 1000U

typedef struct
{
  pwm_channel_id_t number;
  uint16_t duty_cycle;
  uint32_t frequency;
  uint16_t dead_time;
  /* Operational sub-state: only ever UNINITIALIZED, STOPPED or RUNNING.
     Read pwm_get_state() for the effective state (which folds in faults). */
  volatile pwm_state_t op_state;
  /* Latched per-channel OCP fault - the authority, surfaced as FAULTED. */
  volatile bool ocp_fault;
} pwm_channel_t;

extern pwm_channel_t channel_a;
extern pwm_channel_t channel_b;
extern pwm_channel_t channel_c;
extern pwm_channel_t channel_d;
extern pwm_channel_t channel_e;

extern volatile bool pwm_OVP_fault_latched;

void pwm_OVP_fault(void);
void pwm_OCP_fault(pwm_channel_id_t channel);
/* The channel object for an id, or NULL if the id is out of range. For code
 * that works by index (the host command parser) rather than by name. */
pwm_channel_t *pwm_channel(pwm_channel_id_t id);
/* Effective state: FAULTED if an OCP or the global OVP fault is latched,
 * otherwise the channel's operational sub-state. */
pwm_state_t pwm_get_state(const pwm_channel_t *channel);
void pwm_init(pwm_channel_t *channel);
bool pwm_set_duty_cycle(pwm_channel_t *channel, uint16_t duty_cycle);
void pwm_set_dead_time(pwm_channel_t *channel, uint16_t dead_time);
void pwm_set_frequency(pwm_channel_t *channel, uint32_t frequency);
bool pwm_start(pwm_channel_t *channel);
void pwm_stop(pwm_channel_t *channel);
bool pwm_clear_OCP_fault(pwm_channel_t *channel);
bool pwm_clear_OVP_fault(void);

#endif /* PWM_H */
