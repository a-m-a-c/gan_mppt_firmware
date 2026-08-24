#include "stream.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

#define STREAM_PERIOD_MS 1U

/*
Stream packet structure (outgoing):
[id][size][data]
id: 1 byte, identifies the value
size: 1 byte, size of the data field
data: size bytes, little endian
*/

// Wire values. The host decodes on these, so they are not free to renumber.
#define STREAM_ID_VBUS_MV 0x60U
#define STREAM_ID_DUTY    0x61U
#define STREAM_ID_FLAGS   0x62U

#define STREAM_PACKET_COUNT 3U

static uint32_t last_send_ms;
static uint8_t next_packet;

static void put_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

// False if the UART was busy, so the caller retries on the next pass.
static bool send_packet(uint8_t index) {
  uint8_t payload[4];

  switch (index) {
    case 0:
      put_u32(payload, sys.vbus_mv);
      return serial_send(STREAM_ID_VBUS_MV, payload, 4U);
    case 1:
      put_u16(payload, channel_a.pwm.duty_applied);
      return serial_send(STREAM_ID_DUTY, payload, 2U);
    case 2:
      payload[0] = 0U; // No flags defined yet.
      return serial_send(STREAM_ID_FLAGS, payload, 1U);
  }
  return false;
}

void stream_init(void) {
  last_send_ms = HAL_GetTick();
  next_packet = STREAM_PACKET_COUNT; // Idle until the first period elapses.
}

void stream_service(void) {
  const uint32_t now = HAL_GetTick();

  if ((now - last_send_ms) >= STREAM_PERIOD_MS) {
    last_send_ms = now;
    next_packet = 0; // Restart the set. Anything still unsent is abandoned -
                     // a fresh value is worth more than a stale one.
  }

  if (next_packet >= STREAM_PACKET_COUNT) {
    return;
  }

  /* One packet leaves per pass at most, and only when the UART is free. The
     loop runs far faster than the line, so the whole set is out well inside
     the period. */
  if (send_packet(next_packet)) {
    next_packet++;
  }
}
