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

// Run at a start for clean slate.
void command_init(void);

// Returns the mode that should be used based on the latest command received.
mode_t system_command_requested_mode(void);

// To check if a given command has been received. Does not clear the command.
bool system_command_received(system_commands_t command);

// Run every loop.
void command_service(void);

// Run at the end of every loop. Ensures no reuse and removes stale commands.
void command_flush_all(void);

#endif /* COMMAND_H */