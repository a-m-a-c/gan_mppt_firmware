#include "system.h"

system_t sys;

/* Fields are listed explicitly rather than zeroed wholesale: the list is the
   documentation of what the system carries, and a field added to the struct
   without a line here shows up as an obvious gap. */
void system_init(void) {
  sys.vbus_mv = 0U;
  sys.ovp_latched = false;
  sys.state = SYSTEM_STATE_INIT;
  sys.mode = MODE_NONE;
}
