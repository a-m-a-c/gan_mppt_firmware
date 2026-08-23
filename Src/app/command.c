#include "command.h"

#include <stdbool.h>
#include <stdint.h>

#include "main.h"

/* TEMPORARY - stands in for a host until the transports exist.
   Five seconds after the first loop pass this asks once for
   MODE_SINGLE_CH_CV, and system_command_flush_all() consumes it the same way
   the real single slot will. One shot: nothing restarts the mode after it
   exits. Delete the temp_* state with this comment. */
#define COMMAND_TEMP_RUN_DELAY_MS 1000U

static uint32_t temp_start_ms;
static bool temp_started;
static bool temp_fired;

static mode_t pending_mode = MODE_NONE;

mode_t system_command_requested_mode(void) {
    return pending_mode;
}

bool system_command_received(system_commands_t command) {
    return false;
}

void system_command_service(void) {
    // TEMPORARY - see above. Fires once, before STANDBY reads it this pass.
    if (temp_started && !temp_fired &&
        ((HAL_GetTick() - temp_start_ms) >= COMMAND_TEMP_RUN_DELAY_MS)) {
        pending_mode = MODE_SINGLE_CH_CV;
        temp_fired = true;
    }
}

void system_command_flush_all(void) {
    /* TEMPORARY - no init call, so the clock starts on the first pass that
       reaches here. */
    if (!temp_started) {
        temp_start_ms = HAL_GetTick();
        temp_started = true;
    }

    pending_mode = MODE_NONE;
}
