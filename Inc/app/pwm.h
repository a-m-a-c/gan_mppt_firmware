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

typedef enum
{
  PWM_CHANNEL_A = 0,
  PWM_CHANNEL_B,
  PWM_CHANNEL_C,
  PWM_CHANNEL_D,
  PWM_CHANNEL_E,
  PWM_CHANNEL_COUNT
} pwm_channel_id_t;

/* Default pwm_init() settings for bring-up/test code. */
#define PWM_DEFAULT_FREQUENCY_HZ 500000U /* 500 kHz */
#define PWM_DEFAULT_DUTY_CYCLE   500U    /* 50.0% */
#define PWM_DEFAULT_DEAD_TIME_NS 20U     /* 20 ns */

/* Hard limits - every value passed into this module is clamped to these
 * ranges before it reaches the hardware. */
#define PWM_MIN_FREQUENCY_HZ 100000U /* 100 kHz */
#define PWM_MAX_FREQUENCY_HZ 800000U /* 800 kHz */
#define PWM_MIN_DUTY_CYCLE   0U      /* 0.0% */
#define PWM_MAX_DUTY_CYCLE   1000U   /* 100.0% */
#define PWM_MIN_DEAD_TIME_NS 5U      /* 5 ns */
#define PWM_MAX_DEAD_TIME_NS 300U    /* 300 ns */

/* Caller-owned state for one HRTIM half-bridge channel. Set number/frequency/
 * duty_cycle/dead_time before calling pwm_init(); every
 * set_timer_*()/pwm_start()/pwm_stop() call below updates the matching
 * field, so this struct always reflects the channel's last-commanded state.
 *
 * duty_cycle: tenths of a percent, 0-1000 (500 = 50.0%)
 * frequency:  switching frequency in Hz
 * dead_time:  nanoseconds, rounded up to the nearest representable HRTIM tick
 */
typedef struct
{
  pwm_channel_id_t number;
  uint16_t duty_cycle;
  uint32_t frequency;
  uint16_t dead_time;
  /* volatile: both are written from fault ISRs and read by the main loop */
  volatile bool active;
  volatile bool ocp_fault; /* set when this channel's OCP fault (FLT1-5) trips;
                              cleared by pwm_start() / pwm_init() */
} pwm_channel_t;

/* The 5 half-bridge channels, defined in pwm.c. */
extern pwm_channel_t channel_a;
extern pwm_channel_t channel_b;
extern pwm_channel_t channel_c;
extern pwm_channel_t channel_d;
extern pwm_channel_t channel_e;

/* Set by the global OVP fault (OVP pin, EXTI10) after it latches every
 * channel off. Clear in application code before restarting any channel. */
extern volatile bool pwm_global_fault_latched;

/* Latches every channel off (outputs disabled, counters stopped) and sets
 * pwm_global_fault_latched. Called from the OVP interrupt via
 * interrupts.c; safe in ISR context. */
void pwm_global_fault(void);

/* Marks a channel inactive after its OCP fault (FLT1-5) tripped in
 * hardware. Called from the HRTIM fault interrupt via interrupts.c. */
void pwm_channel_fault(pwm_channel_id_t channel);

/* Configures the channel per its number/frequency/duty_cycle/dead_time
 * fields but does not start it - call pwm_start() to enable the counter
 * and outputs. */
void pwm_init(pwm_channel_t *channel);
void pwm_set_duty_cycle(pwm_channel_t *channel, uint16_t duty_cycle);
void pwm_set_dead_time(pwm_channel_t *channel, uint16_t dead_time);
void pwm_set_frequency(pwm_channel_t *channel, uint32_t frequency);
void pwm_start(pwm_channel_t *channel);
void pwm_stop(pwm_channel_t *channel);

#endif /* PWM_H */
