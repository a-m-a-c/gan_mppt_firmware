#include "command.h"



mode_t system_command_requested_mode(void) {
    return MODE_NONE;
}

bool system_command_received(system_commands_t command) {
    return false;
}

void system_command_service(void) {
    // Check for new commands, update internal state.
    return;
}

void system_command_flush_all(void) {
    // flush and reset internal state.
    return;
}
