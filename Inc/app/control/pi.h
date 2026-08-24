#ifndef PI_H
#define PI_H

#include <stdbool.h>


typedef struct {
  /* Configuration. */
  float kp;
  float ki;
  float out_min;
  float out_max;

  /* State */
  float integral;
  float last_error;
  bool clamped;
} pi_t;

void pi_init(pi_t *pi, float kp, float ki, float out_min, float out_max);

void pi_reset(pi_t *pi);

// Returns output, like duty cycle.
float pi_update(pi_t *pi, float setpoint, float measurement, float dt_ms);

void pi_track(pi_t *pi, float applied);

#endif /* PI_H */
