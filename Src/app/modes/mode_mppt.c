#include "mode_mppt.h"

mode_init_result_t mode_mppt_begin(void) {
  // STUB
  return MODE_INIT_OK; // Replace with appropriate return value
}

mode_state_t mode_mppt_service(bool stopping) {
  // STUB - nothing is running yet, so winding down takes no passes at all.
  return stopping ? MODE_STATE_EXIT : MODE_STATE_RUNNING;
}
