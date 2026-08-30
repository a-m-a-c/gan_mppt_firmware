#ifndef MODE_SINGLE_CH_MPPT_H
#define MODE_SINGLE_CH_MPPT_H

#include <stdint.h>

#include "mode.h"

mode_request_result_t mode_single_ch_mppt_begin(void);

mode_state_t mode_single_ch_mppt_service(bool stopping);

/* The P&O setpoint in mV, 0 when the mode is not running. stream.c publishes
   it so the climb can be watched against the measurement it is chasing. */
uint16_t mode_single_ch_mppt_target_mv(void);

#endif /* MODE_SINGLE_CH_MPPT_H */
