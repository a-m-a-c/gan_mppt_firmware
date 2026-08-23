#ifndef COMMAND_H
#define COMMAND_H

#include <stdint.h>
#include <stdbool.h>
#include "mode.h"

typedef enum commands {
    SYSTEM_COMMAND_NONE,
    SYSTEM_COMMAND_RESET,
    SYSTEM_COMMAND_CLEAR_FAULT,
    SYSTEM_COMMAND_STOP,
    SYSTEM_COMMAND_RUN_MPPT,
    SYSTEM_COMMAND_RUN_SINGLE_CH_CV,
    SYSTEM_COMMAND_RUN_SINGLE_CH_MPPT
} system_commands_t;


// Returns the mode that should be used based on the latest command received.
mode_t system_command_requested_mode(void);

// Returns true if a given command has been received.
bool system_command_received(system_commands_t command);

// Check for new commands, update internal state.
void system_command_service(void);

void system_command_flush_all(void);

#endif /* COMMAND_H */