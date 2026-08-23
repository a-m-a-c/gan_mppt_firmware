#include "channel.h"

#include <stddef.h>

#include "config.h"

channel_t channel_a;
channel_t channel_b;
channel_t channel_c;
channel_t channel_d;
channel_t channel_e;

/* For code that iterates - the host reporter walks all five to build its #cfg
   lines. Code that knows which channel it wants should name it directly. */
channel_t *channel_by_id(uint32_t id) {
  switch (id) {
    case CHANNEL_A: return &channel_a;
    case CHANNEL_B: return &channel_b;
    case CHANNEL_C: return &channel_c;
    case CHANNEL_D: return &channel_d;
    case CHANNEL_E: return &channel_e;
    default: break;
  }
  return NULL;
}


/* Every channel gets the same treatment, so this loops over channel_by_id()
   rather than repeating eighteen assignments five times. Fields are listed
   explicitly rather than zeroed wholesale: the list is the documentation of
   what a channel carries, and a field added to the struct without a line here
   shows up as an obvious gap. */
void channel_init_all(void) {
  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++) {
    channel_t *ch = channel_by_id(i);

    ch->id = i;

    ch->telem.vin_v = 0.0f;
    ch->telem.iin_a = 0.0f;
    ch->telem.vout_v = 0.0f;
    ch->telem.iout_a = 0.0f;
    ch->telem.tick_ms = 0U;
    ch->telem.valid = false;

    /* The defaults a channel holds before anything configures it. Duty starts
       at zero - pass-through - for the reasons in config.h; pwm_init() reads
       these back out and programs them, so they must be in place first. */
    ch->pwm.frequency_hz = PWM_DEFAULT_FREQUENCY_HZ;
    ch->pwm.duty_applied = PWM_DEFAULT_DUTY_CYCLE;
    ch->pwm.dead_time_ns = PWM_DEFAULT_DEAD_TIME_NS;
    ch->pwm.op_state = PWM_STATE_UNINITIALIZED;
    ch->pwm.ocp_latched = false;
  }
}
