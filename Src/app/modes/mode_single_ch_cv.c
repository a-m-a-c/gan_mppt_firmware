#include "mode_single_ch_cv.h"

mode_init_result_t mode_single_ch_cv_begin(void) {
  // STUB
  return MODE_INIT_OK;
}

mode_state_t mode_single_ch_cv_service(bool stopping) {
  // STUB - nothing is running yet, so winding down takes no passes at all.
  return stopping ? MODE_STATE_EXIT : MODE_STATE_RUNNING;
}
