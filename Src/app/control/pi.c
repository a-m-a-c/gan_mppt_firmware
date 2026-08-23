#include "pi.h"

void pi_init(pi_t *pi, float kp, float ki, float out_min, float out_max) {
  // STUB
  return;
}

void pi_reset(pi_t *pi) {
  // STUB
  return;
}

float pi_update(pi_t *pi, float setpoint, float measurement, float dt_s) {
  // STUB
  return 0.0f;
}

void pi_track(pi_t *pi, float applied) {
  // STUB
  return;
}
