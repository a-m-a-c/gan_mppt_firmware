#include "mode.h"
#include <stdbool.h>

#include "mode_mppt.h"
#include "mode_single_ch_cv.h"
#include "mode_single_ch_iv_sweep.h"
#include "mode_single_ch_mppt.h"

static mode_t active_mode = MODE_NONE;
static bool stopping = false;

/* Starts a mode and hands it the five channels.

   Responsibilities:

   - Latch the mode. Every later call acts on the one latched here.
   - Initialise fully, never assuming a clean slate. A previous run may have
     left an integrator wound up, a ramp part way through, or a channel
     selected, and none of it may survive into this one. This is the half of
     the safety story that the pwm_stop_all() in app.c cannot cover: that stops
     the hardware, this clears the software.
   - Refuse synchronously. MODE_INIT_REFUSED comes back from this same call, so
     a transport can answer its host without waiting for a later pass.
   - Clean up after itself. Never return MODE_INIT_FAULT or MODE_INIT_REFUSED
     with a channel still switching - app.c does no teardown on those paths. */
mode_request_result_t mode_begin(mode_t mode) {
  active_mode = mode;
  stopping = false;

  switch (mode) {
    case MODE_MPPT:
      return mode_mppt_begin();
    case MODE_SINGLE_CH_CV:
      return mode_single_ch_cv_begin();
    case MODE_SINGLE_CH_IV_SWEEP:
      return mode_single_ch_iv_sweep_begin();
    case MODE_SINGLE_CH_MPPT:
      return mode_single_ch_mppt_begin();
    case MODE_NONE:
      break;
  }

  return MODE_INIT_REFUSED;
}

/* Runs the latched mode for one pass and reports where it has got to.

   Responsibilities:

   - Never block. One pass of work and return, like every other service in the
     loop.
   - Own all five channels while the system is ACTIVE. Nothing outside this
     module commands a channel; requests arrive here and the mode decides what
     to do with them.
   - Hold the stop request. stop_request is true for exactly one pass; it is
     latched into `stopping` and passed down as a level, so the mode below sees
     it on every pass until it exits.
   - Own the teardown. MODE_STATE_EXIT and MODE_STATE_FAULTED both mean the
     channels are already stopped and the internal state is already reset.
     app.c tears nothing down; the pwm_stop_all() on entry to STANDBY and
     FAULTED is a backstop against a buggy mode, not the mechanism.

   Stopping is allowed to take several passes - ramping down to zero duty is
   the reason for asking rather than telling - and MODE_STATE_RUNNING is the
   honest answer until it lands. */
mode_state_t mode_service(bool stop_request) {
  if (stop_request) stopping = true;

  switch (active_mode) {
    case MODE_MPPT:
      return mode_mppt_service(stopping);
    case MODE_SINGLE_CH_CV:
      return mode_single_ch_cv_service(stopping);
    case MODE_SINGLE_CH_IV_SWEEP:
      return mode_single_ch_iv_sweep_service(stopping);
    case MODE_SINGLE_CH_MPPT:
      return mode_single_ch_mppt_service(stopping);
    case MODE_NONE:
      break;
  }

  return MODE_STATE_EXIT;
}
