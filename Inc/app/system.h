#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdbool.h>
#include <stdint.h>
#include "mode.h"
/* Board-wide state, as against the per-channel state in channel.h. Anything
   here is true of the whole converter, not of one channel. */

typedef enum {
  SYSTEM_STATE_INIT,
  SYSTEM_STATE_CHECK,
  SYSTEM_STATE_STANDBY,
  SYSTEM_STATE_ACTIVE,
  SYSTEM_STATE_FAULTED,
  SYSTEM_STATE_RESET
} system_state_t;

typedef struct
{
  uint32_t vbus_mv; /* battery bus, from the ADC divider - shared by all channels */
  volatile bool ovp_latched; /* ISR-written; global, hence here not per channel */
  system_state_t state;
  mode_t mode;
} system_t;

extern system_t sys;

/* Puts every field in a known state. Call once from app_setup(), before
   anything reads sys. */
void system_init(void);

#endif /* SYSTEM_H */
