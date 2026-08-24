#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>
#include "transport.h"

void serial_init(void);

// Should be called before command_service()
// Checks IRQ flags set by callbacks and updates the command queue.
void serial_service(void);

// Return false if queue is empty.
bool serial_take_next_frame(transport_frame_t *frame);

void serial_rx_complete(void);
void serial_rx_error(void);

/* Sends one [id][size][data] packet. Non-blocking, and refuses rather than
   queueing - false means a transfer was already in flight and nothing was
   sent. */
bool serial_send(uint8_t id, const uint8_t *data, uint8_t len);

#endif /* SERIAL_H */