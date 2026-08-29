#include "command.h"

#include <stdbool.h>
#include <stdint.h>
#include "transport.h"
#include "main.h"
#include "serial.h"

#define NUM_COMMAND_SLOTS 8 // Number of slots in system_commands_t

typedef enum {
  OP_SYSTEM_COMMAND_NONE = 0x00,
  OP_SYSTEM_COMMAND_RESET = 0x01,
  OP_SYSTEM_COMMAND_CLEAR_FAULT = 0x02,
  OP_SYSTEM_COMMAND_STOP = 0x03,
  OP_SYSTEM_COMMAND_RUN_MPPT = 0x04,
  OP_SYSTEM_COMMAND_RUN_SINGLE_CH_CV = 0x05,
  OP_SYSTEM_COMMAND_RUN_SINGLE_CH_MPPT = 0x06,
  OP_SYSTEM_COMMAND_RUN_SINGLE_CH_IV_SWEEP = 0x07
} serial_opcode_t;

static mode_t pending_mode;

static bool system_command_received_register[NUM_COMMAND_SLOTS];

static void accept_serial_command(transport_frame_t *frame) {
  switch (frame->op) {
    case OP_SYSTEM_COMMAND_RESET:
      system_command_received_register[SYSTEM_COMMAND_RESET] = true;
      break;
    case OP_SYSTEM_COMMAND_CLEAR_FAULT:
      system_command_received_register[SYSTEM_COMMAND_CLEAR_FAULT] = true;
      break;
    case OP_SYSTEM_COMMAND_STOP:
      system_command_received_register[SYSTEM_COMMAND_STOP] = true;
      pending_mode = MODE_NONE;
      break;
    case OP_SYSTEM_COMMAND_RUN_MPPT:
      system_command_received_register[SYSTEM_COMMAND_RUN_MPPT] = true;
      pending_mode = MODE_MPPT;
      break;
    case OP_SYSTEM_COMMAND_RUN_SINGLE_CH_CV:
      system_command_received_register[SYSTEM_COMMAND_RUN_SINGLE_CH_CV] = true;
      pending_mode = MODE_SINGLE_CH_CV;
      break;
    case OP_SYSTEM_COMMAND_RUN_SINGLE_CH_MPPT:
      system_command_received_register[SYSTEM_COMMAND_RUN_SINGLE_CH_MPPT] = true;
      pending_mode = MODE_SINGLE_CH_MPPT;
      break;
    case OP_SYSTEM_COMMAND_RUN_SINGLE_CH_IV_SWEEP:
      system_command_received_register[SYSTEM_COMMAND_RUN_SINGLE_CH_IV_SWEEP] = true;
      pending_mode = MODE_SINGLE_CH_IV_SWEEP;
      break;
    default:
      // Unknown command, ignore
      break;
  }
}

// Run at start, may have additional logic later.
void command_init(void) {
  command_flush_all();
}

// Checks for the most recent run command received.
mode_t system_command_requested_mode(void) {
    return pending_mode;
}

// Checks if a command has been received. Does not clear.
bool system_command_received(system_commands_t command) {
    if (command >= NUM_COMMAND_SLOTS) return false;
    return system_command_received_register[command];
}

// Checks drivers for new commands. Serial takes priority. Can later.
void command_service(void) {
  transport_frame_t frame;
  while (serial_take_next_frame(&frame)) {
    accept_serial_command(&frame);
  }
}

// Clear all commands. Reset state.
void command_flush_all(void) {
  pending_mode = MODE_NONE;
  for (int i = 0; i < NUM_COMMAND_SLOTS; i++) {
    system_command_received_register[i] = false;
  }
}
