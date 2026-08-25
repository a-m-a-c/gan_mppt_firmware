#include "mode_single_ch_mppt.h"

mode_request_result_t mode_single_ch_mppt_begin(void) {
  // STUB
  return MODE_INIT_OK;
}

mode_state_t mode_single_ch_mppt_service(bool stopping) {
  // STUB - nothing is running yet, so winding down takes no passes at all.
  return stopping ? MODE_STATE_EXIT : MODE_STATE_RUNNING;
}
