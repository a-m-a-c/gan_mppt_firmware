#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>

/* One validated frame, whatever carried it. Transports do framing only - they
   never interpret op or data. The opcode map lives in command.c so serial and
   FDCAN cannot drift apart. */

// 8 because a classic CAN frame carries 8 data bytes and both transports must
// carry the same message set. FDCAN_FRAME_CLASSIC is set in fdcan.c.
#define TRANSPORT_MAX_PAYLOAD 8U

typedef struct {
  uint8_t op;
  uint8_t len;                            // bytes used in data
  uint8_t data[TRANSPORT_MAX_PAYLOAD];
} transport_frame_t;

#endif /* TRANSPORT_H */
