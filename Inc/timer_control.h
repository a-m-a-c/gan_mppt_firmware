/**
  ******************************************************************************
  * @file    timer_control.h
  * @author  Angus Macdonald
  * @brief   Timer control functions.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef TIMER_CONTROL_H
#define TIMER_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  TIMER_CHANNEL_A = 0,
  TIMER_CHANNEL_B,
  TIMER_CHANNEL_C,
  TIMER_CHANNEL_D,
  TIMER_CHANNEL_E,
  TIMER_CHANNEL_COUNT
} timer_channel_id_t;

/* Default channel_timer_init() settings for bring-up/test code. */
#define TIMER_CONTROL_DEFAULT_FREQUENCY_HZ 500000U /* 500 kHz */
#define TIMER_CONTROL_DEFAULT_DUTY_CYCLE   500U    /* 50.0% */
#define TIMER_CONTROL_DEFAULT_DEAD_TIME_NS 20U     /* 20 ns */

/* Caller-owned state for one HRTIM half-bridge channel. Set number/frequency/
 * duty_cycle/dead_time before calling channel_timer_init(); every
 * set_timer_*()/start_timer()/stop_timer() call below updates the matching
 * field, so this struct always reflects the channel's last-commanded state.
 *
 * duty_cycle: tenths of a percent, 0-1000 (500 = 50.0%)
 * frequency:  switching frequency in Hz
 * dead_time:  nanoseconds, rounded up to the nearest representable HRTIM tick
 */
typedef struct
{
  timer_channel_id_t number;
  uint16_t duty_cycle;
  uint32_t frequency;
  uint16_t dead_time;
  bool active;
} timer_channel_t;

/* The 5 half-bridge channels, defined in timer_control.c. */
extern timer_channel_t channel_a;
extern timer_channel_t channel_b;
extern timer_channel_t channel_c;
extern timer_channel_t channel_d;
extern timer_channel_t channel_e;

/* Configures the channel per its number/frequency/duty_cycle/dead_time
 * fields but does not start it - call start_timer() to enable the counter
 * and outputs. */
void channel_timer_init(timer_channel_t *channel);
void set_timer_duty_cycle(timer_channel_t *channel, uint16_t duty_cycle);
void set_timer_deadtime(timer_channel_t *channel, uint16_t dead_time);
void set_timer_frequency(timer_channel_t *channel, uint32_t frequency);
void start_timer(timer_channel_t *channel);
void stop_timer(timer_channel_t *channel);

#endif /* TIMER_CONTROL_H */
